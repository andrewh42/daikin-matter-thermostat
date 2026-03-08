/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#pragma once

#include "s21_datalink.h"

#include <hal/nrf_uarte.h>
#include <zephyr/kernel.h>

#include <atomic>
#include <cstdint>

/**
 * S21 data-link layer over nRF54L UARTE with hardware DMA pattern matching.
 *
 * Uses three DPPI channels:
 *   Channel A: ENDTX → STARTRX + TIMER20 START  (TX-to-RX turnaround; start watchdog)
 *   Channel B: MATCH[ETX/ACK/NAK] + TIMER20 COMPARE[0] → STOPRX  (frame termination or timeout)
 *   Channel C: ENDRX → TIMER20 STOP  (cancel watchdog on normal RX completion)
 *
 * The ISR uses a four-state state machine (Idle → WaitingRx → TimedOut/SendingAck → Idle)
 * to ensure exactly one k_msgq_put and one k_work_submit per transaction, preventing
 * deadlocks from simultaneous ENDRX+ERROR events or cross-ISR FRAME_TIMEOUT races.
 * See README-S21-UART-design.md for full design documentation.
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

    // ── ISR hardware state machine ──
    // See README-S21-UART-design.md for state diagram and transition table.
    enum class IsrState : uint8_t {
        Idle,           // no active transaction; ignore all events
        WaitingRx,      // TX complete (DPPI chained STARTRX), awaiting response
        TimedOut,       // timeout detected, waiting for ENDRX to deliver final DMA result
        SendingAck,     // valid transact response received, ACK byte in flight
    };

    // ── Operation state ──
    enum class OpType : uint8_t {
        None,
        Send,
        Transact
    };

    NRF_UARTE_Type* m_uarte;
    uint8_t mGppiChTxRx;             // ENDTX → STARTRX + TIMER START         (Ch A)
    uint8_t mGppiChMatchOrTimeout;   // MATCH + TIMER COMPARE[0] → STOPRX     (Ch B)
    uint8_t mGppiChEndrxTimer;       // ENDRX → TIMER STOP                    (Ch C)

    // Standard-layout wrapper so we can recover `this` from a k_work* without
    // using offsetof on a non-standard-layout type (avoids -Winvalid-offsetof).
    struct CompletionWork {
        struct k_work work; // must remain first
        S21DataLinkUart* self;
    };

    struct k_msgq m_rxMsgq;
    char __aligned(4) m_rxMsgqBuf[sizeof(RxResult)]; // depth = 1

    CompletionWork m_completionWork;

    std::atomic<IsrState> m_isrState{IsrState::Idle};
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
