/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#pragma once

#include "S21DataLink.h"

#include <hal/nrf_uarte.h>
#include <zephyr/kernel.h>

#include <atomic>
#include <cstdint>

/**
 * S21 data-link layer over nRF54L UARTE with hardware DMA pattern matching.
 *
 * Uses two DPPI channels:
 *   Channel A: ENDTX → STARTRX  (automatic TX-to-RX turnaround)
 *   Channel B: MATCH  → STOPRX  (frame termination on ETX/ACK/NAK)
 *
 * The only CPU wakeup per transaction is the ENDRX ISR, which posts the
 * result to a k_msgq and submits a k_work.  The callback runs in the
 * system work-queue thread.
 */
class S21DataLinkUart: public S21DataLink {
  public:
    explicit S21DataLinkUart(NRF_UARTE_Type* uarte);

    /// Configure UARTE, MATCH candidates, DPPI, and IRQ.
    /// @param txPin  nrf_gpio pin number for TX (e.g. NRF_GPIO_PIN_MAP(2,8)).
    /// @param rxPin  nrf_gpio pin number for RX.
    /// @return 0 on success, negative errno on failure.
    int init(uint32_t txPin, uint32_t rxPin);

    void send(std::vector<std::byte> payload, SendCallback cb) override;
    void transact(std::vector<std::byte> payload, TransactCallback cb) override;

  private:
    // ── S21 framing bytes ──
    static constexpr uint8_t kETX = 0x03;
    static constexpr uint8_t kACK = 0x06;
    static constexpr uint8_t kNAK = 0x15;

    // ── MATCH filter indices ──
    static constexpr uint8_t kMatchIdxETX = 0;
    static constexpr uint8_t kMatchIdxACK = 1;
    static constexpr uint8_t kMatchIdxNAK = 2;

    static constexpr size_t kBufSize = 32;

    // At 2400 baud, 8E2: 12 bits/char ≈ 5ms/char.
    // 240 bit periods = 100ms idle gap — generous for inter-byte/initial response.
    static constexpr uint32_t kFrameTimeoutBits = 240;

    // ── ISR → work-queue result ──
    enum class RxStatus : uint8_t {
        Ok,
        UartError,
        Timeout,
    };

    struct RxResult {
        uint8_t data[kBufSize];
        uint8_t len;
        RxStatus status;
    };

    // ── Operation state ──
    enum class OpType : uint8_t {
        None,
        Send,
        Transact
    };

    NRF_UARTE_Type* m_uarte;
    uint8_t m_dppiChTxRx;  // ENDTX → STARTRX
    uint8_t m_dppiChMatch; // MATCH → STOPRX

    // Standard-layout wrapper so we can recover `this` from a k_work* without
    // using offsetof on a non-standard-layout type (avoids -Winvalid-offsetof).
    struct CompletionWork {
        struct k_work work; // must remain first
        S21DataLinkUart* self;
    };

    struct k_msgq m_rxMsgq;
    char __aligned(4) m_rxMsgqBuf[sizeof(RxResult)]; // depth = 1

    CompletionWork m_completionWork;

    std::atomic<OpType> m_opType{OpType::None};
    SendCallback m_sendCb;
    TransactCallback m_transactCb;

    uint8_t __aligned(4) m_txBuf[kBufSize];
    uint8_t __aligned(4) m_rxBuf[kBufSize];

    void configureMatch(bool etx, bool ack, bool nak);
    void startTransaction(const std::vector<std::byte>& encoded, bool matchEtx, bool matchAck);
    const char* rxMatch();

    static void isrHandler(const void* arg);
    static const char* rxResultError(RxResult& result);
    static void completionWorkHandler(struct k_work* work);
};
