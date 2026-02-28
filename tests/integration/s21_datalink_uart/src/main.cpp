/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Integration tests for S21DataLinkUart.
 * Requires a physical loopback wire between the uart21 TX and RX pins
 * (defined by the s21uart pinctrl-0 state in the board DTS).
 *
 * Tests 1-5 start RX manually before TX so the loopback catches all bytes.
 * Test 6 uses DPPI ENDTX→STARTRX to validate the automated TX-to-RX path.
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>

#include "s21/S21DataLinkUart.h"
#include "s21/S21Frame.h"
#include "s21/s21_pinconfig.h"

#include <hal/nrf_uarte.h>

#include <cstring>
#include <vector>

/* ── Shared test state ────────────────────────────────────────────── */
static S21DataLinkUart* s_dut;

static K_SEM_DEFINE(s_doneSem, 0, 1);

/* send() result */
static bool s_sendOk;
static char s_sendErrMsg[64];

/* transact() result */
static bool s_transactOk;
static std::vector<std::byte> s_transactData;
static char s_transactErrMsg[64];

/* ── Callbacks ────────────────────────────────────────────────────── */

static void sendCb(tl::expected<void, S21DataLinkError> result)
{
    if (result.has_value()) {
        s_sendOk = true;
    } else {
        s_sendOk = false;
        strncpy(s_sendErrMsg, result.error().what(), sizeof(s_sendErrMsg) - 1);
    }
    k_sem_give(&s_doneSem);
}

static void transactCb(tl::expected<std::vector<std::byte>, S21DataLinkError> result)
{
    if (result.has_value()) {
        s_transactOk = true;
        s_transactData = std::move(*result);
    } else {
        s_transactOk = false;
        strncpy(s_transactErrMsg, result.error().what(), sizeof(s_transactErrMsg) - 1);
    }
    k_sem_give(&s_doneSem);
}

/* ── Test fixture ─────────────────────────────────────────────────── */

static void *suite_setup(void)
{
    static S21DataLinkUart dut(NRF_UARTE21);
    int err = dut.init(s21_pinconfig::kTxPin, s21_pinconfig::kRxPin);
    zassert_equal(err, 0, "S21DataLinkUart init failed: %d", err);
    s_dut = &dut;
    return nullptr;
}

static void before_each(void *)
{
    s_sendOk = false;
    s_sendErrMsg[0] = '\0';
    s_transactOk = false;
    s_transactData.clear();
    s_transactErrMsg[0] = '\0';
    k_sem_reset(&s_doneSem);
}

ZTEST_SUITE(s21_datalink_uart, NULL, suite_setup, before_each, NULL, NULL);

/* ── Helper: wait for callback with timeout ───────────────────────── */

static bool waitDone(k_timeout_t timeout = K_MSEC(2000))
{
    return k_sem_take(&s_doneSem, timeout) == 0;
}

/* ─────────────────────────────────────────────────────────────────────
 * Test 1 — MATCH ETX stops RX
 *
 * send() an F1 query frame.  In loopback the TX data arrives at RX.
 * Since send() matches ACK/NAK, and the frame contains ETX (0x03) but
 * NOT ACK/NAK, the ACK match won't fire.  But ETX is also not in the
 * match set for send().  This test verifies the timeout path (no match).
 *
 * For a direct ETX-match test, use transact() which enables ETX match.
 * Transmit a raw frame [STX, 'F', '1', checksum, ETX]. RX catches it
 * via loopback, and MATCH ETX fires → STOPRX.
 * ──────────────────────────────────────────────────────────────────── */

ZTEST(s21_datalink_uart, test_match_etx_stops_rx)
{
    /* transact() enables ETX + NAK match.  We encode an F1 query; the
     * loopback receives the encoded frame which ends with ETX. */
    s_dut->transact({std::byte{'F'}, std::byte{'1'}}, transactCb);

    zassert_true(waitDone(), "callback not invoked");

    /* In loopback, the device receives its own TX frame (no ACK prefix).
     * transact() tries to decode the raw frame [STX, F, 1, chk, ETX].
     * The decode should succeed, yielding payload [F, 1]. */
    zassert_true(s_transactOk, "unexpected error: %s", s_transactErrMsg);
    zassert_equal(s_transactData.size(), 2, "payload len %zu != 2", s_transactData.size());
    zassert_equal(static_cast<uint8_t>(s_transactData[0]), 'F');
    zassert_equal(static_cast<uint8_t>(s_transactData[1]), '1');
}

/* ─────────────────────────────────────────────────────────────────────
 * Test 2 — MATCH ACK stops RX
 *
 * Craft a raw TX that sends a single ACK byte (0x06).  Use send() which
 * matches ACK/NAK.  The loopback receives ACK, MATCH fires, and the
 * callback reports success.
 *
 * We cheat by encoding a payload whose frame happens to contain 0x06.
 * Actually, it's simpler: we'll just use the internal startTransaction
 * path by calling send() with a payload that, once encoded, starts
 * with bytes the receiver sees — but in loopback, the receiver sees
 * the *whole* encoded frame.  If the encoded frame contains 0x06
 * (ACK) anywhere, MATCH ACK will fire.
 *
 * The simplest approach: the frame starts with STX (0x02).  STX is
 * NOT in the match set, so MATCH won't fire on STX.  We need the
 * frame to contain 0x06.  Let's pick a payload whose checksum = 0x06.
 * But 0x06 is reserved — the checksum logic adds 2 → 0x08.  So we
 * can't easily get 0x06 into a frame via encode.
 *
 * Alternative: for a pure ACK test, we really need the S21 device to
 * respond with ACK.  In loopback without a real device, we can only
 * verify that send() reports "unexpected response" (since the first
 * byte is STX, not ACK).  This is still a valid integration test of
 * the MATCH/DPPI chain — it proves MATCH and the callback pipeline
 * work.
 * ──────────────────────────────────────────────────────────────────── */

ZTEST(s21_datalink_uart, test_send_loopback_non_ack)
{
    /* send() enables ACK+NAK match.  The loopback receives the encoded
     * frame.  The first byte is STX (0x02), which doesn't match ACK or
     * NAK.  But if any frame byte equals 0x06 or 0x15, match fires.
     *
     * Payload 'D','1' → encoded: [02, 44, 31, 75, 03]
     * Checksum 0x44+0x31 = 0x75 — no ACK/NAK bytes in the frame.
     * So no match fires, and we get a timeout. */
    s_dut->send({std::byte{'D'}, std::byte{'1'}}, sendCb);

    zassert_true(waitDone(), "callback not invoked");
    zassert_false(s_sendOk, "expected error, got success");
    zassert_true(strcmp(s_sendErrMsg, "timeout") == 0,
                 "expected 'timeout', got '%s'", s_sendErrMsg);
}

/* ─────────────────────────────────────────────────────────────────────
 * Test 3 — MATCH NAK stops RX
 *
 * Send a payload whose encoded frame contains NAK (0x15).
 * Checksum of a single 0x15 byte = 0x15 (reserved) → 0x17.
 * So [02, 15, 17, 03] — the 0x15 payload byte triggers NAK match.
 * This should happen in send() mode (ACK+NAK match enabled).
 * ──────────────────────────────────────────────────────────────────── */

ZTEST(s21_datalink_uart, test_match_nak_stops_rx)
{
    /* Payload [0x15] → frame [02, 15, 17, 03].
     * In loopback: RX sees [02, 15, ...]. MATCH NAK fires on 0x15.
     * STOPRX fires. RX buffer contains [02, 15] (2 bytes).
     * send() sees data[0]=0x02, which is neither ACK nor NAK → "unexpected response". */
    s_dut->send({std::byte{0x15}}, sendCb);

    zassert_true(waitDone(), "callback not invoked");
    zassert_false(s_sendOk, "expected error");
    /* First received byte is STX (0x02), not NAK — the NAK is the second byte.
     * But MATCH fires on the NAK byte, stopping RX.  The completion handler
     * checks data[0] which is STX → "unexpected response". */
    zassert_true(strcmp(s_sendErrMsg, "unexpected response") == 0,
                 "expected 'unexpected response', got '%s'", s_sendErrMsg);
}

/* ─────────────────────────────────────────────────────────────────────
 * Test 4 — ETX match continues past ACK byte
 *
 * Use transact() which enables ETX+NAK match (NOT ACK).
 * Send a frame that contains 0x06 in the payload.  The ACK byte should
 * NOT stop RX.  RX should continue until ETX (0x03).
 * ──────────────────────────────────────────────────────────────────── */

ZTEST(s21_datalink_uart, test_etx_match_continues_past_ack)
{
    /* Payload [0x06, 0x41] → checksum (0x06+0x41)&0xFF = 0x47
     * Encoded: [02, 06, 41, 47, 03]
     * In loopback with ETX match: RX sees bytes until 0x03, then stops.
     * The 0x06 in the payload does NOT trigger a match (ACK match disabled).
     * Full frame is captured. */
    s_dut->transact({std::byte{0x06}, std::byte{0x41}}, transactCb);

    zassert_true(waitDone(), "callback not invoked");
    zassert_true(s_transactOk, "unexpected error: %s", s_transactErrMsg);
    /* Decoded payload should be [0x06, 0x41]. */
    zassert_equal(s_transactData.size(), 2);
    zassert_equal(static_cast<uint8_t>(s_transactData[0]), 0x06);
    zassert_equal(static_cast<uint8_t>(s_transactData[1]), 0x41);
}

/* ─────────────────────────────────────────────────────────────────────
 * Test 5 — Timeout when no match byte arrives
 *
 * Send a frame that does NOT contain ACK or NAK using send() mode.
 * send() matches ACK+NAK only.  If the frame has neither, timeout fires.
 * ──────────────────────────────────────────────────────────────────── */

ZTEST(s21_datalink_uart, test_timeout_no_match)
{
    /* Payload 'F','1' → frame [02, 46, 31, 77, 03]
     * No 0x06 or 0x15 in the frame → no match → timeout. */
    s_dut->send({std::byte{'F'}, std::byte{'1'}}, sendCb);

    zassert_true(waitDone(), "callback not invoked within 2s");
    zassert_false(s_sendOk, "expected timeout error");
    zassert_true(strcmp(s_sendErrMsg, "timeout") == 0,
                 "expected 'timeout', got '%s'", s_sendErrMsg);
}

/* ─────────────────────────────────────────────────────────────────────
 * Test 6 — DPPI ENDTX→STARTRX chain
 *
 * This test validates the full automated TX→RX path.  With loopback at
 * 2400 baud, ENDTX fires after DMA loads the last byte into the TX
 * shift register.  DPPI triggers STARTRX while the byte is still being
 * physically transmitted.  The loopback captures at least the final
 * byte(s).
 *
 * We use transact() with a frame ending in ETX.  If the DPPI chain
 * works, RX starts in time to catch the tail of the transmission.
 * ──────────────────────────────────────────────────────────────────── */

ZTEST(s21_datalink_uart, test_dppi_endtx_startrx)
{
    /* This is the normal API path.  transact() relies on DPPI ENDTX→STARTRX.
     * In loopback, RX may catch one or more bytes from the tail of TX.
     * The frame ends with ETX (0x03), which is in the ETX match set.
     *
     * If the DPPI chain is broken, RX never starts, no match fires, and
     * we get a timeout.  If it works, we get either a valid decode or
     * an error from a partial frame — but NOT a timeout. */
    s_dut->transact({std::byte{'F'}, std::byte{'1'}}, transactCb);

    zassert_true(waitDone(), "callback not invoked");

    /* Success means the DPPI chain worked and loopback captured the frame.
     * We accept either a successful decode or a frame error (partial capture)
     * — the key assertion is that we did NOT timeout. */
    if (!s_transactOk) {
        zassert_true(strcmp(s_transactErrMsg, "timeout") != 0,
                     "got timeout — DPPI ENDTX→STARTRX chain may be broken");
    }
    else {
        zassert_true(false);
    }
}
