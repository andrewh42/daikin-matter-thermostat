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

LOG_MODULE_REGISTER(s21_uart, LOG_LEVEL_DBG);

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

    /* Clear relevant events. */
    nrf_uarte_event_clear(m_uarte, NRF_UARTE_EVENT_ENDTX);
    nrf_uarte_event_clear(m_uarte, NRF_UARTE_EVENT_ENDRX);
    nrf_uarte_event_clear(m_uarte, NRF_UARTE_EVENT_ERROR);
    m_uarte->EVENTS_DMA.RX.MATCH[kMatchIdxETX] = 0;
    m_uarte->EVENTS_DMA.RX.MATCH[kMatchIdxACK] = 0;
    m_uarte->EVENTS_DMA.RX.MATCH[kMatchIdxNAK] = 0;
    k_msgq_purge(&m_rxMsgq);

    /* Clear FRAMETIMEOUT event before starting. */
    nrf_uarte_event_clear(m_uarte, NRF_UARTE_EVENT_FRAME_TIMEOUT);

    /* Reset watchdog timer so it starts from 0 when ENDTX fires (Ch A). */
    nrf_timer_task_trigger(NRF_TIMER20, NRF_TIMER_TASK_CLEAR);
    nrf_timer_event_clear(NRF_TIMER20, NRF_TIMER_EVENT_COMPARE0);

    /* Fire TX — DPPI will chain: ENDTX → STARTRX + TIMER START → MATCH/COMPARE → STOPRX → ENDRX. */
    nrf_uarte_task_trigger(m_uarte, NRF_UARTE_TASK_STARTTX);
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
 * ISR — only wakeup per transaction
 * ────────────────────────────────────────────────────────────────────── */

void S21DataLinkUart::isrHandler(const void* arg)
{
    auto* self = const_cast<S21DataLinkUart*>(reinterpret_cast<const S21DataLinkUart*>(arg));
    NRF_UARTE_Type* uarte = self->m_uarte;

    if (nrf_uarte_event_check(uarte, NRF_UARTE_EVENT_ENDRX)) {
        nrf_uarte_event_clear(uarte, NRF_UARTE_EVENT_ENDRX);

        bool timeout = nrf_timer_event_check(NRF_TIMER20, NRF_TIMER_EVENT_COMPARE0);

        if (timeout) {
            nrf_timer_event_clear(NRF_TIMER20, NRF_TIMER_EVENT_COMPARE0);
            LOG_DBG("S21DataLinkUart: RX timeout (no response)");
        } else if (nrf_uarte_event_check(uarte, NRF_UARTE_EVENT_FRAME_TIMEOUT)) {
            nrf_uarte_event_clear(uarte, NRF_UARTE_EVENT_FRAME_TIMEOUT);
            LOG_DBG("S21DataLinkUart: RX timeout (frame)");
            timeout = true;
        } else {
            LOG_DBG("S21DataLinkUart: RX ended with %s match", self->rxMatch());
        }

        RxResult result{};
        result.len = static_cast<uint8_t>(nrf_uarte_rx_amount_get(uarte));
        std::memcpy(result.data, self->m_rxBuf, result.len);
        result.status = timeout ? RxStatus::Timeout : RxStatus::Ok;

        /* For transact operations with a valid frame (not a NAK), send ACK back to the AC. */
        bool nakReceived = uarte->EVENTS_DMA.RX.MATCH[kMatchIdxNAK] > 0;
        bool sendingAck = !timeout && !nakReceived && self->m_opType.load(std::memory_order_relaxed) == OpType::Transact;
        if (sendingAck) {
            LOG_DBG("S21DataLinkUart: sending ACK for transact response");

            uint32_t domain_id = nrfx_gppi_domain_id_get((uint32_t)uarte);
            nrfx_gppi_channels_disable(domain_id, BIT(self->mGppiChTxRx));
            self->m_txBuf[0] = kACK;
            nrf_uarte_event_clear(uarte, NRF_UARTE_EVENT_ENDTX);
            nrf_uarte_tx_buffer_set(uarte, self->m_txBuf, 1);
            nrf_uarte_int_enable(uarte, NRF_UARTE_INT_ENDTX_MASK);
            nrf_uarte_task_trigger(uarte, NRF_UARTE_TASK_STARTTX);
        }

        k_msgq_put(&self->m_rxMsgq, &result, K_NO_WAIT);
        /* When ACK is being transmitted, defer k_work_submit to the ENDTX
         * handler so that Ch A (ENDTX→STARTRX) is re-enabled *before* the
         * completion callback runs.  This prevents a new transaction from
         * calling STARTTX while Ch A is still disabled. */
        if (!sendingAck) {
            k_work_submit(&self->m_completionWork.work);
        }
    }

    if (nrf_uarte_event_check(uarte, NRF_UARTE_EVENT_ERROR)) {
        nrf_uarte_event_clear(uarte, NRF_UARTE_EVENT_ERROR);
        uint32_t errSrc = nrf_uarte_errorsrc_get_and_clear(uarte);
        LOG_ERR("UARTE RX error source: 0x%08x %s%s%s%s", errSrc,
            (errSrc & UARTE_ERRORSRC_OVERRUN_Msk) ? "overrun " : "",
            (errSrc & UARTE_ERRORSRC_PARITY_Msk) ? "parity " : "",
            (errSrc & UARTE_ERRORSRC_FRAMING_Msk) ? "framing " : "",
            (errSrc & UARTE_ERRORSRC_BREAK_Msk) ? "break " : "");

        /* Stop any ongoing RX. */
        nrf_uarte_task_trigger(uarte, NRF_UARTE_TASK_STOPRX);

        RxResult result{};
        result.len = 0;
        result.status = RxStatus::UartError;
        k_msgq_put(&self->m_rxMsgq, &result, K_NO_WAIT);
        k_work_submit(&self->m_completionWork.work);
    }

    /* FRAMETIMEOUT without ENDRX — clear it to avoid spurious interrupts. */
    if (nrf_uarte_event_check(uarte, NRF_UARTE_EVENT_FRAME_TIMEOUT)) {
        nrf_uarte_event_clear(uarte, NRF_UARTE_EVENT_FRAME_TIMEOUT);
    }

    /* ENDTX — ACK byte transmitted.
     * Re-enable Ch A (ENDTX→STARTRX) *before* notifying the completion
     * handler so that a new transaction can safely call STARTTX immediately
     * from within the callback without racing against the Ch A enable. */
    if (nrf_uarte_event_check(uarte, NRF_UARTE_EVENT_ENDTX)) {
        nrf_uarte_event_clear(uarte, NRF_UARTE_EVENT_ENDTX);
        nrf_uarte_int_disable(uarte, NRF_UARTE_INT_ENDTX_MASK);
        uint32_t domain_id = nrfx_gppi_domain_id_get((uint32_t)uarte);
        nrfx_gppi_channels_enable(domain_id, BIT(self->mGppiChTxRx));
        k_work_submit(&self->m_completionWork.work);
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
        LOG_ERR("completion: msgq empty");
        self->m_opType.store(OpType::None);
        return;
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
}
