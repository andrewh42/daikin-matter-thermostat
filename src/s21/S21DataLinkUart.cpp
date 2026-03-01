/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#include "S21DataLinkUart.h"
#include "S21Frame.h"

#include <hal/nrf_gpio.h>
#include <hal/nrf_uarte.h>
#include <helpers/nrfx_gppi.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>

#include <cstring>

LOG_MODULE_REGISTER(s21_uart, CONFIG_CHIP_APP_LOG_LEVEL);

S21DataLinkUart::S21DataLinkUart(NRF_UARTE_Type* uarte)
        : m_uarte(uarte)
        , m_dppiChTxRx(0)
        , m_dppiChMatch(0)
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
    m_dppiChTxRx = static_cast<uint8_t>(ch);

    ch = nrfx_gppi_channel_alloc(domain_id);
    if (ch < 0) {
        LOG_ERR("DPPI alloc ch B failed: %d", ch);
        nrfx_gppi_channel_free(domain_id, m_dppiChTxRx);
        return ch;
    }
    m_dppiChMatch = static_cast<uint8_t>(ch);

    /* --- Channel A: ENDTX → STARTRX ------------------------------- */
    nrf_uarte_publish_set(m_uarte, NRF_UARTE_EVENT_ENDTX, m_dppiChTxRx);
    nrf_uarte_subscribe_set(m_uarte, NRF_UARTE_TASK_STARTRX, m_dppiChTxRx);
    nrfx_gppi_channels_enable(domain_id, BIT(m_dppiChTxRx));

    /* --- Channel B: MATCH[0..2] → STOPRX -------------------------- */
    /* MATCH events have no HAL enum — set publish registers directly. */
    m_uarte->PUBLISH_DMA.RX.MATCH[kMatchIdxETX] = m_dppiChMatch | NRF_SUBSCRIBE_PUBLISH_ENABLE;
    m_uarte->PUBLISH_DMA.RX.MATCH[kMatchIdxACK] = m_dppiChMatch | NRF_SUBSCRIBE_PUBLISH_ENABLE;
    m_uarte->PUBLISH_DMA.RX.MATCH[kMatchIdxNAK] = m_dppiChMatch | NRF_SUBSCRIBE_PUBLISH_ENABLE;
    nrf_uarte_subscribe_set(m_uarte, NRF_UARTE_TASK_STOPRX, m_dppiChMatch);
    nrfx_gppi_channels_enable(domain_id, BIT(m_dppiChMatch));

    /* --- IRQ: ENDRX + ERROR --------------------------------------- */
    nrf_uarte_int_enable(m_uarte,
                         NRF_UARTE_INT_ENDRX_MASK | NRF_UARTE_INT_ERROR_MASK | NRF_UARTE_INT_FRAME_TIMEOUT_MASK);

    unsigned int irqn = DT_IRQ(DT_ALIAS(s21uart), irq);
    irq_connect_dynamic(irqn, DT_IRQ(DT_ALIAS(s21uart), priority), isrHandler, reinterpret_cast<const void*>(this), 0);
    irq_enable(irqn);

    /* --- Enable UARTE --------------------------------------------- */
    nrf_uarte_enable(m_uarte);

    LOG_INF("S21DataLinkUart init OK (DPPI %u/%u)", m_dppiChTxRx, m_dppiChMatch);
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

    /* Fire TX — DPPI will chain: ENDTX → STARTRX → MATCH → STOPRX → ENDRX. */
    nrf_uarte_task_trigger(m_uarte, NRF_UARTE_TASK_STARTTX);
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

        bool timeout = nrf_uarte_event_check(uarte, NRF_UARTE_EVENT_FRAME_TIMEOUT);
        if (timeout) {
            nrf_uarte_event_clear(uarte, NRF_UARTE_EVENT_FRAME_TIMEOUT);
        }

        RxResult result{};
        result.len = static_cast<uint8_t>(nrf_uarte_rx_amount_get(uarte));
        std::memcpy(result.data, self->m_rxBuf, result.len);
        result.status = timeout ? RxStatus::Timeout : RxStatus::Ok;
        k_msgq_put(&self->m_rxMsgq, &result, K_NO_WAIT);
        k_work_submit(&self->m_completionWork.work);
    }

    if (nrf_uarte_event_check(uarte, NRF_UARTE_EVENT_ERROR)) {
        nrf_uarte_event_clear(uarte, NRF_UARTE_EVENT_ERROR);
        uint32_t errSrc = nrf_uarte_errorsrc_get_and_clear(uarte);
        LOG_ERR("UARTE error: 0x%08x", errSrc);

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
