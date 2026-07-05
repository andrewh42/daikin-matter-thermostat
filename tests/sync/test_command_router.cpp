/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Host tests for the pure SetpointRaiseLower arithmetic that backs the
 * Thermostat command handler in command_router.cpp. The CommandHandlerInterface
 * glue itself is CHIP-flavoured and not host-testable; the math is.
 */
#include "setpoint_math.h"

#include <catch2/catch_test_macros.hpp>

using sync_aai::applySetpointDelta;

TEST_CASE("applySetpointDelta scales amount by 10 (0.1C steps -> 0.01C units)", "[setpoint]")
{
    // +20 steps = +2.0 C = +200 (0.01 C) on a 25.00 C setpoint.
    REQUIRE(applySetpointDelta(/*current=*/2500, /*amount=*/20, /*lo=*/0, /*hi=*/5000) == 2700);
    // Negative amount lowers.
    REQUIRE(applySetpointDelta(2500, -20, 0, 5000) == 2300);
    // Zero is a no-op.
    REQUIRE(applySetpointDelta(2500, 0, 0, 5000) == 2500);
}

TEST_CASE("applySetpointDelta clamps to the limit window", "[setpoint]")
{
    // Raise past the upper limit clamps to hi.
    REQUIRE(applySetpointDelta(/*current=*/3100, /*amount=*/20, /*lo=*/1600, /*hi=*/3200) == 3200);
    // Lower past the bottom limit clamps to lo.
    REQUIRE(applySetpointDelta(/*current=*/1700, /*amount=*/-20, /*lo=*/1600, /*hi=*/3200) == 1600);
    // Landing exactly on a limit is allowed (inclusive).
    REQUIRE(applySetpointDelta(3000, 20, 1600, 3200) == 3200);
}

TEST_CASE("applySetpointDelta computes in 32-bit to avoid int16 overflow", "[setpoint]")
{
    // current near INT16_MAX with a positive amount must not wrap negative
    // before clamping; it clamps to hi.
    REQUIRE(applySetpointDelta(/*current=*/32760, /*amount=*/127, /*lo=*/0, /*hi=*/32767) == 32767);
    // Symmetric guard at the bottom.
    REQUIRE(applySetpointDelta(/*current=*/-32760, /*amount=*/-127, /*lo=*/-32768, /*hi=*/0) == -32768);
}
