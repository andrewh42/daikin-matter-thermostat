/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Phase 8 — Reachable host tests
 * ------------------------------
 * The "supervisor" half (consecutive-failure counter) lives in
 * AirConditionerManager and runs on the Zephyr work queue; it's
 * exercised on-device. These tests pin the reconciler half of the
 * contract: the reachable twin must move correctly through the
 * (false → true → false → true) cycle in response to successful
 * observations and explicit link-down notifications.
 */

#include <catch2/catch_test_macros.hpp>

#include "reconciler.h"

using namespace sync;

namespace {

S21State okPoll()
{
    return S21State{true, OperatingMode::Cool, 2400, FanMode::Auto, 2300, 1500, 50};
}

} // namespace

TEST_CASE("Reachable: false at boot defaults", "[phase8][reachable]")
{
    LogicalACState s;
    REQUIRE_FALSE(s.reachable.observed());
}

TEST_CASE("Reachable: first successful observation flips to true",
          "[phase8][reachable]")
{
    ManualTimeSource time;
    LogicalACState   state;
    Reconciler       rec(state, time);

    auto change = rec.applyObservation(okPoll());

    REQUIRE(state.reachable.observed());
    // No specific assertion on dirty paths until the BDBI cluster is
    // added in ZAP — the projector currently doesn't emit Reachable
    // dirty markers. The twin transition itself is enough.
    (void)change;
}

TEST_CASE("Reachable: applying observation after explicit link-down flips back true",
          "[phase8][reachable]")
{
    ManualTimeSource time;
    LogicalACState   state;
    Reconciler       rec(state, time);

    rec.applyObservation(okPoll());
    REQUIRE(state.reachable.observed());

    // Simulate the supervisor's NotifyLinkDown path.
    state.reachable.applyObservation(false, ObservationSource::Device);
    REQUIRE_FALSE(state.reachable.observed());

    rec.applyObservation(okPoll());
    REQUIRE(state.reachable.observed());
}
