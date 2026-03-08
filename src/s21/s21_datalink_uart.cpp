/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#include "s21_datalink_uart.h"
#include "s21_frame.h"

#include <hal/nrf_gpio.h>
#include <hal/nrf_timer.h>
#include <hal/nrf_uarte.h>
#include <helpers/nrfx_gppi.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>

#include <cstring>

LOG_MODULE_REGISTER(s21_uart, LOG_LEVEL_INF);

S21DataLinkUart::S21DataLinkUart(NRF_UARTE_Type* uarte)
        : m_uarte(uarte)
        , mGppiChTxRx(0)
        , mGppiChMatchOrTimeout(0)
        , mGppiChEndrxTimer(0)
        , m_rxMsgq{}
        , m_rxMsgqBuf{}
        , m_completionWork{}
        , m_sendCb(nullptr)
        , m_transactCb(nullptr)
        , m_txBuf{}
        , m_rxBuf{}
{
}

int S21DataLinkUart::init(uint32_t txPin, uint32_t rxPin)
{
    /* --- k_msgq / k_work ------------------------------------------- */
    k_msgq_init(&m_rxMsgq, m_rxMsgqBuf, sizeof(RxResult), /*max_msgs=*/1);
    m_completionWork.self = this;
    k_work_init(&m_completionWork.work, completionWorkHandler);

    /* --- Disable UARTE while reconfiguring ------------------------- */
    nrf_uarte_disable(m_uarte);

    /* --- Pin configuration ----------------------------------------- */
    nrf_gpio_pin_set(txPin);
    nrf_gpio_cfg_output(txPin);
    nrf_gpio_cfg_input(rxPin, NRF_GPIO_PIN_PULLUP);
    nrf_uarte_txrx_pins_set(m_uarte, txPin, rxPin);

    /* --- UART parameters: 2400 / 8E2 / no HWFC -------------------- */
    nrf_uarte_baudrate_set(m_uarte, NRF_UARTE_BAUDRATE_2400);
    nrf_uarte_config_t cfg = {
            .hwfc = NRF_UARTE_HWFC_DISABLED,
            .parity = NRF_UARTE_PARITY_INCLUDED,
            .stop = NRF_UARTE_STOP_TWO,
            .frame_timeout = NRF_UARTE_FRAME_TIMEOUT_EN,
    };
    nrf_uarte_configure(m_uarte, &cfg);
    nrf_uarte_frame_timeout_set(m_uarte, kFrameTimeoutBits);
    nrf_uarte_shorts_enable(m_uarte, NRF_UARTE_SHORT_FRAME_TIMEOUT_STOPRX);

    /* --- MATCH candidates (fixed, enable bits toggled per-op) ------ */
    m_uarte->DMA.RX.MATCH.CANDIDATE[kMatchIdxETX] = kETX;
    m_uarte->DMA.RX.MATCH.CANDIDATE[kMatchIdxACK] = kACK;
    m_uarte->DMA.RX.MATCH.CANDIDATE[kMatchIdxNAK] = kNAK;
    configureMatch(false, false, false); /* all off until a transaction */

    /* --- Allocate two DPPI channels in the peripheral domain ------- */
    /* Derive the domain ID from the UARTE instance address so we don't   */
    /* hard-code DPPIC20; nrfx_gppi_channel_alloc/free require the same  */
    /* node_id that nrfx_gppi_domain_id_get() returns for this peripheral. */
    uint32_t domain_id = nrfx_gppi_domain_id_get((uint32_t)m_uarte);
    int ch;

    ch = nrfx_gppi_channel_alloc(domain_id);
    if (ch < 0) {
        LOG_ERR("DPPI alloc ch A failed: %d", ch);
        return ch;
    }
    mGppiChTxRx = static_cast<uint8_t>(ch);

    ch = nrfx_gppi_channel_alloc(domain_id);
    if (ch < 0) {
        LOG_ERR("DPPI alloc ch B failed: %d", ch);
        nrfx_gppi_channel_free(domain_id, mGppiChTxRx);
        return ch;
    }
    mGppiChMatchOrTimeout = static_cast<uint8_t>(ch);

    /* --- Channel A: ENDTX → STARTRX ------------------------------- */
    nrf_uarte_publish_set(m_uarte, NRF_UARTE_EVENT_ENDTX, mGppiChTxRx);
    nrf_uarte_subscribe_set(m_uarte, NRF_UARTE_TASK_STARTRX, mGppiChTxRx);
    nrfx_gppi_channels_enable(domain_id, BIT(mGppiChTxRx));

    /* --- Channel B: MATCH[0..2] → STOPRX -------------------------- */
    /* MATCH events have no HAL enum — set publish registers directly. */
    m_uarte->PUBLISH_DMA.RX.MATCH[kMatchIdxETX] = mGppiChMatchOrTimeout | NRF_SUBSCRIBE_PUBLISH_ENABLE;
    m_uarte->PUBLISH_DMA.RX.MATCH[kMatchIdxACK] = mGppiChMatchOrTimeout | NRF_SUBSCRIBE_PUBLISH_ENABLE;
    m_uarte->PUBLISH_DMA.RX.MATCH[kMatchIdxNAK] = mGppiChMatchOrTimeout | NRF_SUBSCRIBE_PUBLISH_ENABLE;
    nrf_uarte_subscribe_set(m_uarte, NRF_UARTE_TASK_STOPRX, mGppiChMatchOrTimeout);
    nrfx_gppi_channels_enable(domain_id, BIT(mGppiChMatchOrTimeout));

    /* --- TIMER20: one-shot 200 ms watchdog for no-response timeout -- */
    /* Prescaler 4 → 16 MHz / 2^4 = 1 MHz → 1 µs/tick; 200 000 ticks = 200 ms. */
    nrf_timer_mode_set(NRF_TIMER20, NRF_TIMER_MODE_TIMER);
    nrf_timer_bit_width_set(NRF_TIMER20, NRF_TIMER_BIT_WIDTH_32);
    nrf_timer_prescaler_set(NRF_TIMER20, 4);
    nrf_timer_cc_set(NRF_TIMER20, NRF_TIMER_CC_CHANNEL0, 200000);
    nrf_timer_shorts_enable(NRF_TIMER20, NRF_TIMER_SHORT_COMPARE0_STOP_MASK);

    /* Ch A addition: TIMER START subscribes to ENDTX (alongside STARTRX). */
    nrf_timer_subscribe_set(NRF_TIMER20, NRF_TIMER_TASK_START, mGppiChTxRx);

    /* Ch B addition: TIMER COMPARE[0] also publishes to Ch B (→ STOPRX). */
    nrf_timer_publish_set(NRF_TIMER20, NRF_TIMER_EVENT_COMPARE0, mGppiChMatchOrTimeout);

    /* Ch C: ENDRX → TIMER STOP (cancel watchdog on normal RX completion). */
    ch = nrfx_gppi_channel_alloc(domain_id);
    if (ch < 0) {
        LOG_ERR("DPPI alloc ch C failed: %d", ch);
        nrfx_gppi_channel_free(domain_id, mGppiChTxRx);
        nrfx_gppi_channel_free(domain_id, mGppiChMatchOrTimeout);
        return ch;
    }
    mGppiChEndrxTimer = static_cast<uint8_t>(ch);
    nrf_uarte_publish_set(m_uarte, NRF_UARTE_EVENT_ENDRX, mGppiChEndrxTimer);
    nrf_timer_subscribe_set(NRF_TIMER20, NRF_TIMER_TASK_STOP, mGppiChEndrxTimer);
    nrfx_gppi_channels_enable(domain_id, BIT(mGppiChEndrxTimer));

    /* --- IRQ: ENDRX + ERROR --------------------------------------- */
    nrf_uarte_int_enable(m_uarte,
                         NRF_UARTE_INT_ENDRX_MASK | NRF_UARTE_INT_ERROR_MASK | NRF_UARTE_INT_FRAME_TIMEOUT_MASK);

    unsigned int irqn = DT_IRQ(DT_ALIAS(s21uart), irq);
    irq_connect_dynamic(irqn, DT_IRQ(DT_ALIAS(s21uart), priority), isrHandler, reinterpret_cast<const void*>(this), 0);
    irq_enable(irqn);

    /* --- Enable UARTE --------------------------------------------- */
    nrf_uarte_enable(m_uarte);

    LOG_INF("S21DataLinkUart init OK (DPPI %u/%u/%u)", mGppiChTxRx, mGppiChMatchOrTimeout, mGppiChEndrxTimer);
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────
 * MATCH filter configuration
 * ────────────────────────────────────────────────────────────────────── */

void S21DataLinkUart::configureMatch(bool etx, bool ack, bool nak)
{
    uint32_t cfg = 0;
    if (etx) {
        cfg |= UARTE_DMA_RX_MATCH_CONFIG_ENABLE0_Msk;
    }
    if (ack) {
        cfg |= UARTE_DMA_RX_MATCH_CONFIG_ENABLE1_Msk;
    }
    if (nak) {
        cfg |= UARTE_DMA_RX_MATCH_CONFIG_ENABLE2_Msk;
    }
    m_uarte->DMA.RX.MATCH.CONFIG = cfg;
}

/* ──────────────────────────────────────────────────────────────────────
 * Public API: send / transact
 * ────────────────────────────────────────────────────────────────────── */

void S21DataLinkUart::send(std::vector<std::byte> payload, SendCallback cb)
{
    OpType expected = OpType::None;
    if (!m_opType.compare_exchange_strong(expected, OpType::Send)) {
        cb(tl::unexpected(S21DataLinkError("busy")));
        return;
    }
    m_sendCb = std::move(cb);

    auto encoded = S21Frame::encode(payload);
    startTransaction(encoded, /*matchEtx=*/false, /*matchAck=*/true);
}

void S21DataLinkUart::transact(std::vector<std::byte> payload, TransactCallback cb)
{
    OpType expected = OpType::None;
    if (!m_opType.compare_exchange_strong(expected, OpType::Transact)) {
        cb(tl::unexpected(S21DataLinkError("busy")));
        return;
    }
    m_transactCb = std::move(cb);

    auto encoded = S21Frame::encode(payload);
    startTransaction(encoded, /*matchEtx=*/true, /*matchAck=*/false);
}

/* ──────────────────────────────────────────────────────────────────────
 * Start a TX→RX transaction (common path)
 * ────────────────────────────────────────────────────────────────────── */

void S21DataLinkUart::startTransaction(const std::vector<std::byte>& encoded, bool matchEtx, bool matchAck)
{
    /* Enable the appropriate MATCH filters (NAK always on). */
    configureMatch(matchEtx, matchAck, /*nak=*/true);

    /* Set up DMA buffers. */
    nrf_uarte_rx_buffer_set(m_uarte, m_rxBuf, sizeof(m_rxBuf));
    size_t txLen = (encoded.size() <= sizeof(m_txBuf)) ? encoded.size() : sizeof(m_txBuf);
    std::memcpy(m_txBuf, encoded.data(), txLen);
    nrf_uarte_tx_buffer_set(m_uarte, m_txBuf, txLen);

    /* Disable UARTE IRQ while clearing events and setting state to prevent
     * a stale ENDRX (from a previous ERROR handler's STOPRX) from firing
     * between event clears and STARTTX and being misprocessed as belonging
     * to this new transaction. */
    unsigned int irqn = DT_IRQ(DT_ALIAS(s21uart), irq);
    irq_disable(irqn);

    /* Clear all events. */
    nrf_uarte_event_clear(m_uarte, NRF_UARTE_EVENT_ENDTX);
    nrf_uarte_event_clear(m_uarte, NRF_UARTE_EVENT_ENDRX);
    nrf_uarte_event_clear(m_uarte, NRF_UARTE_EVENT_ERROR);
    nrf_uarte_event_clear(m_uarte, NRF_UARTE_EVENT_FRAME_TIMEOUT);
    m_uarte->EVENTS_DMA.RX.MATCH[kMatchIdxETX] = 0;
    m_uarte->EVENTS_DMA.RX.MATCH[kMatchIdxACK] = 0;
    m_uarte->EVENTS_DMA.RX.MATCH[kMatchIdxNAK] = 0;
    k_msgq_purge(&m_rxMsgq);

    /* Reset watchdog timer so it starts from 0 when ENDTX fires (Ch A). */
    nrf_timer_task_trigger(NRF_TIMER20, NRF_TIMER_TASK_CLEAR);
    nrf_timer_event_clear(NRF_TIMER20, NRF_TIMER_EVENT_COMPARE0);

    m_isrState.store(IsrState::WaitingRx, std::memory_order_relaxed);

    /* Fire TX — DPPI will chain: ENDTX → STARTRX + TIMER START → MATCH/COMPARE → STOPRX → ENDRX. */
    nrf_uarte_task_trigger(m_uarte, NRF_UARTE_TASK_STARTTX);

    irq_enable(irqn);
}

/// @brief  Helper to identify which MATCH candidate caused the STOPRX (for logging/debugging).
/// @return The name of the matched candidate, or "no" if none (e.g. RX timeout).
const char* S21DataLinkUart::rxMatch() {
    const char* match;
    // simplifying assumption: match events are mutually exclusive because of the two modes, send and transact:
    //   - send mode means we're expecting an ACK/NAK, and the S21 protocol will send one or the other, not both.
    //   - transact mode means we're expecting an ETX)
    if (m_uarte->EVENTS_DMA.RX.MATCH[kMatchIdxETX] > 0) {
        match = "ETX";
    }
    else if (m_uarte->EVENTS_DMA.RX.MATCH[kMatchIdxACK] > 0) {
        match = "ACK";
    }
    else if (m_uarte->EVENTS_DMA.RX.MATCH[kMatchIdxNAK] > 0) {
        match = "NAK";
    } else {
        match = "no";
    }
    return match;
}

/* ──────────────────────────────────────────────────────────────────────
 * ISR — state machine driven
 *
 * Event processing order: FRAME_TIMEOUT → ENDRX → ERROR → ENDTX.
 * See README-S21-UART-design.md for state diagram and transition table.
 * ────────────────────────────────────────────────────────────────────── */

void S21DataLinkUart::isrHandler(const void* arg)
{
    auto* self = const_cast<S21DataLinkUart*>(reinterpret_cast<const S21DataLinkUart*>(arg));
    NRF_UARTE_Type* uarte = self->m_uarte;

    /* Cache state locally; update both local and m_isrState on transitions
     * so later handlers in the same invocation see the updated state. */
    auto state = self->m_isrState.load(std::memory_order_relaxed);

    /* ── FRAME_TIMEOUT ──────────────────────────────────────────────── */
    /* Checked first so that a same-invocation FRAME_TIMEOUT transitions
     * the state to TimedOut before the ENDRX handler reads it. */
    if (nrf_uarte_event_check(uarte, NRF_UARTE_EVENT_FRAME_TIMEOUT)) {
        nrf_uarte_event_clear(uarte, NRF_UARTE_EVENT_FRAME_TIMEOUT);
        if (state == IsrState::WaitingRx) {
            LOG_INF("S21DataLinkUart: RX timeout (frame)");
            state = IsrState::TimedOut;
            self->m_isrState.store(state, std::memory_order_relaxed);
        }
    }

    /* ── ENDRX ──────────────────────────────────────────────────────── */
    if (nrf_uarte_event_check(uarte, NRF_UARTE_EVENT_ENDRX)) {
        nrf_uarte_event_clear(uarte, NRF_UARTE_EVENT_ENDRX);

        if (state == IsrState::TimedOut) {
            /* Timeout already determined by state — put result directly. */
            RxResult result{};
            result.len = static_cast<uint8_t>(nrf_uarte_rx_amount_get(uarte));
            std::memcpy(result.data, self->m_rxBuf, result.len);
            result.status = RxStatus::Timeout;

            k_msgq_put(&self->m_rxMsgq, &result, K_NO_WAIT);
            state = IsrState::Idle;
            self->m_isrState.store(state, std::memory_order_relaxed);
            k_work_submit(&self->m_completionWork.work);

        } else if (state == IsrState::WaitingRx) {
            /* Check TIMER20 COMPARE0 as fallback for 200ms no-response
             * watchdog.  (TIMER20 is on the SERIAL20 IRQ domain and cannot
             * transition the state directly without a second ISR.) */
            bool timeout = nrf_timer_event_check(NRF_TIMER20, NRF_TIMER_EVENT_COMPARE0);
            if (timeout) {
                nrf_timer_event_clear(NRF_TIMER20, NRF_TIMER_EVENT_COMPARE0);
                LOG_INF("S21DataLinkUart: RX timeout (no response)");
            } else {
                LOG_DBG("S21DataLinkUart: RX ended with %s match", self->rxMatch());
            }

            /* Build result. */
            RxResult result{};
            result.len = static_cast<uint8_t>(nrf_uarte_rx_amount_get(uarte));
            std::memcpy(result.data, self->m_rxBuf, result.len);
            result.status = timeout ? RxStatus::Timeout : RxStatus::Ok;

            bool nakReceived = uarte->EVENTS_DMA.RX.MATCH[kMatchIdxNAK] > 0;
            bool needsAck = !timeout && !nakReceived
                            && self->m_opType.load(std::memory_order_relaxed) == OpType::Transact;

            /* Put result first, then conditionally send ACK. */
            k_msgq_put(&self->m_rxMsgq, &result, K_NO_WAIT);

            if (needsAck) {
                /* Disable Ch A so ACK TX doesn't chain into STARTRX. */
                LOG_DBG("S21DataLinkUart: sending ACK for transact response");
                uint32_t domain_id = nrfx_gppi_domain_id_get((uint32_t)uarte);
                nrfx_gppi_channels_disable(domain_id, BIT(self->mGppiChTxRx));
                self->m_txBuf[0] = kACK;
                nrf_uarte_event_clear(uarte, NRF_UARTE_EVENT_ENDTX);
                nrf_uarte_tx_buffer_set(uarte, self->m_txBuf, 1);
                nrf_uarte_int_enable(uarte, NRF_UARTE_INT_ENDTX_MASK);
                nrf_uarte_task_trigger(uarte, NRF_UARTE_TASK_STARTTX);

                state = IsrState::SendingAck;
                self->m_isrState.store(state, std::memory_order_relaxed);
            } else {
                state = IsrState::Idle;
                self->m_isrState.store(state, std::memory_order_relaxed);
                k_work_submit(&self->m_completionWork.work);
            }

        } else {
            /* SendingAck or Idle: stale ENDRX (from prior STOPRX). Ignore. */
            LOG_DBG("S21DataLinkUart: ENDRX in state %u, ignoring", static_cast<unsigned>(state));
        }
    }

    /* ── ERROR ──────────────────────────────────────────────────────── */
    if (nrf_uarte_event_check(uarte, NRF_UARTE_EVENT_ERROR)) {
        nrf_uarte_event_clear(uarte, NRF_UARTE_EVENT_ERROR);
        uint32_t errSrc = nrf_uarte_errorsrc_get_and_clear(uarte);
        LOG_WRN("UARTE RX error source: 0x%08x %s%s%s%s", errSrc,
            (errSrc & UARTE_ERRORSRC_OVERRUN_Msk) ? "overrun " : "",
            (errSrc & UARTE_ERRORSRC_PARITY_Msk) ? "parity " : "",
            (errSrc & UARTE_ERRORSRC_FRAMING_Msk) ? "framing " : "",
            (errSrc & UARTE_ERRORSRC_BREAK_Msk) ? "break " : "");

        if (state == IsrState::WaitingRx) {
            nrf_uarte_task_trigger(uarte, NRF_UARTE_TASK_STOPRX);

            RxResult result{};
            result.len = 0;
            result.status = RxStatus::UartError;
            k_msgq_put(&self->m_rxMsgq, &result, K_NO_WAIT);

            state = IsrState::Idle;
            self->m_isrState.store(state, std::memory_order_relaxed);
            k_work_submit(&self->m_completionWork.work);
        }
        /* SendingAck, TimedOut, or Idle: result already queued or pending.
         * Error source is logged above; no further action needed. */
    }

    /* ── ENDTX ──────────────────────────────────────────────────────── */
    /* ACK byte transmitted. Re-enable Ch A (ENDTX→STARTRX) *before*
     * submitting work so that a new transaction can safely call STARTTX
     * immediately from within the callback. */
    if (nrf_uarte_event_check(uarte, NRF_UARTE_EVENT_ENDTX)) {
        nrf_uarte_event_clear(uarte, NRF_UARTE_EVENT_ENDTX);
        nrf_uarte_int_disable(uarte, NRF_UARTE_INT_ENDTX_MASK);

        if (state == IsrState::SendingAck) {
            uint32_t domain_id = nrfx_gppi_domain_id_get((uint32_t)uarte);
            nrfx_gppi_channels_enable(domain_id, BIT(self->mGppiChTxRx));

            self->m_isrState.store(IsrState::Idle, std::memory_order_relaxed);
            k_work_submit(&self->m_completionWork.work);
        }
        /* Other states: stale ENDTX, ignore. */
    }
}

const char* S21DataLinkUart::rxResultError(RxResult& result)
{
    if (result.status == RxStatus::Timeout) {
        return "timeout";
    }
    else if (result.status != RxStatus::Ok) {
        return "uart error";
    }
    else if (result.len < 1) {
        return "no response";
    }
    else if (result.data[0] == kNAK) {
        return "NAK";
    }
    else {
        return nullptr;
    }
}

/* ──────────────────────────────────────────────────────────────────────
 * Completion handler — runs in system work-queue thread
 * ────────────────────────────────────────────────────────────────────── */

void S21DataLinkUart::completionWorkHandler(struct k_work* work)
{
    // CompletionWork is standard-layout and `work` is its first member,
    // so this reinterpret_cast is well-defined (no offsetof needed).
    auto* self = reinterpret_cast<CompletionWork*>(work)->self;

    RxResult result{};
    if (k_msgq_get(&self->m_rxMsgq, &result, K_NO_WAIT) != 0) {
        LOG_ERR("completion: msgq empty (ISR state machine invariant violated)");
        __ASSERT(false, "completionWorkHandler: msgq empty");
        /* In release builds: synthesise an error so the callback fires
         * and k_sem_give is called, preventing a deadlock. */
        result.len = 0;
        result.status = RxStatus::UartError;
    }

    /* Disable match filters until the next operation. */
    self->configureMatch(false, false, false);

    OpType op = self->m_opType.exchange(OpType::None);

    if (op == OpType::Send) {
        auto cb = std::move(self->m_sendCb);
        if (!cb) {
            return;
        }

        auto resultError = rxResultError(result);
        if (resultError) {
            cb(tl::unexpected(S21DataLinkError(resultError)));
        }
        else if (result.data[0] != kACK) {
            cb(tl::unexpected(S21DataLinkError("unexpected response")));
        }
        else {
            cb({});
        }
    }
    else if (op == OpType::Transact) {
        auto cb = std::move(self->m_transactCb);
        if (!cb) {
            return;
        }

        auto resultError = rxResultError(result);
        if (resultError) {
            cb(tl::unexpected(S21DataLinkError(resultError)));
            return;
        }

        /* RX buffer: [ACK, STX, payload, checksum, ETX] or [STX, ...] */
        size_t frameStart = (result.data[0] == kACK) ? 1 : 0;
        std::vector<std::byte> frame(reinterpret_cast<std::byte*>(result.data) + frameStart,
                                     reinterpret_cast<std::byte*>(result.data) + result.len);
        auto decoded = S21Frame::decode(frame);
        if (!decoded) {
            cb(tl::unexpected(S21DataLinkError(decoded.error().what())));
        }
        else {
            cb(std::move(*decoded));
        }
    }
    else {
        LOG_WRN("completion: opType was None (stale work submit?)");
    }
}
