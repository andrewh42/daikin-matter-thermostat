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

S21OperationalObservation okOperationalObservation()
{
    return {true, OperatingMode::Cool, 2400, FanMode::Auto, std::nullopt};
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

    auto change = rec.applyOperationalObservation(okOperationalObservation());

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

    rec.applyOperationalObservation(okOperationalObservation());
    REQUIRE(state.reachable.observed());

    // Simulate the supervisor's NotifyLinkDown path.
    state.reachable.applyObservation(false);
    REQUIRE_FALSE(state.reachable.observed());

    rec.applyOperationalObservation(okOperationalObservation());
    REQUIRE(state.reachable.observed());
}

TEST_CASE("Reachable: environmental observation alone does not flip reachable",
          "[phase8][reachable][c1-split]")
{
    // Reachable is anchored on the op heartbeat. An env observation
    // arriving without a preceding op observation must not paper over a
    // link-down — the next op tick is the authoritative signal.
    ManualTimeSource time;
    LogicalACState   state;
    Reconciler       rec(state, time);

    REQUIRE_FALSE(state.reachable.observed());

    rec.applyEnvironmentalObservation({2300, 1500, 50});
    REQUIRE_FALSE(state.reachable.observed());
}
