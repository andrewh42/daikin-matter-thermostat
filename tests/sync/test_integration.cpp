/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Phase 7 integration host test
 * -----------------------------
 * Drives a real Reconciler through scripted sequences of (observation,
 * intent, observation, intent, …) and asserts the resulting D1 sequence
 * and dirty-attribute set. Pins the end-to-end choreography that the
 * on-device pump and AAI Write/Read paths rely on.
 */

#include <catch2/catch_test_macros.hpp>

#include "reconciler.h"

#include <algorithm>
#include <vector>

using namespace sync;

namespace {

/// Records the sequence of (sendCommand, dirty-attribute count) pairs
/// produced by a series of Reconciler operations. The test then asserts
/// against this transcript rather than per-call.
struct Transcript {
    struct Entry {
        std::optional<S21OperationCommand> sendCommand;
        size_t                             dirtyCount;
        std::vector<LogicalAttribute>      dirty;
    };
    std::vector<Entry> entries;
    void record(const OperationalChange& c) {
        entries.push_back({c.sendCommand, c.dirtyAttributes.size(), c.dirtyAttributes});
    }
};

bool transcriptContainsAttribute(const Transcript& t, LogicalAttribute attr, size_t entryIdx)
{
    if (entryIdx >= t.entries.size()) return false;
    return std::any_of(t.entries[entryIdx].dirty.begin(),
                       t.entries[entryIdx].dirty.end(),
                       [&](LogicalAttribute a) { return a == attr; });
}

S21OperationalObservation opPoll(bool onOff, OperatingMode mode, int16_t setpoint,
                                 FanMode fan = FanMode::Auto)
{
    return {onOff, mode, setpoint, fan, std::nullopt};
}

} // namespace

// ─── Scripted scenario ───────────────────────────────────────────────────────

TEST_CASE("Integration: poll/intent/poll/intent/poll converges with one D1 per intent",
          "[phase7][integration]")
{
    ManualTimeSource time;
    LogicalACState   state(LogicalACStateDefaults{
        .onOff = true, .mode = OperationalMode::Cool, .coolSetpoint = 2400});
    Reconciler       rec(state, time);

    Transcript t;

    // Step 0: first poll establishes the device baseline.
    t.record(rec.applyOperationalObservation(opPoll(true, OperatingMode::Cool, 2400)));
    REQUIRE_FALSE(t.entries[0].sendCommand.has_value());

    // Step 1: controller raises cool setpoint to 26.
    t.record(rec.applyIntent(SetOccupiedCoolingSetpointIntent{2600}));
    REQUIRE(t.entries[1].sendCommand.has_value());
    REQUIRE(t.entries[1].sendCommand->setpointCelsius == 2600);
    rec.onCommandSent(*t.entries[1].sendCommand);

    // Step 2: confirming poll — device returns 26. No second D1.
    t.record(rec.applyOperationalObservation(opPoll(true, OperatingMode::Cool, 2600)));
    REQUIRE_FALSE(t.entries[2].sendCommand.has_value());

    // Step 3: controller flips mode to Heat. Setpoint switches to the
    // heat shadow (LogicalACStateDefaults default 2000).
    t.record(rec.applyIntent(SetSystemModeIntent{true, OperationalMode::Heat}));
    REQUIRE(t.entries[3].sendCommand.has_value());
    REQUIRE(t.entries[3].sendCommand->operatingMode  == OperatingMode::Heat);
    REQUIRE(t.entries[3].sendCommand->setpointCelsius == 2000);
    REQUIRE(transcriptContainsAttribute(t, LogicalAttribute::SystemMode, 3));
    rec.onCommandSent(*t.entries[3].sendCommand);

    // Step 4: confirming Heat-mode poll. No D1.
    t.record(rec.applyOperationalObservation(opPoll(true, OperatingMode::Heat, 2000)));
    REQUIRE_FALSE(t.entries[4].sendCommand.has_value());
}

TEST_CASE("Integration: stale poll arriving after fresh write doesn't kick a re-send",
          "[phase7][integration]")
{
    ManualTimeSource time;
    LogicalACState   state(LogicalACStateDefaults{
        .onOff = true, .mode = OperationalMode::Cool, .coolSetpoint = 2400});
    Reconciler       rec(state, time);
    rec.applyOperationalObservation(opPoll(true, OperatingMode::Cool, 2400));

    auto change = rec.applyIntent(SetOccupiedCoolingSetpointIntent{2600});
    rec.onCommandSent(*change.sendCommand);

    // Stale poll: still reads 2400. Reconciler should leave state alone
    // and not produce a fresh D1.
    auto staleChange = rec.applyOperationalObservation(opPoll(true, OperatingMode::Cool, 2400));
    REQUIRE_FALSE(staleChange.sendCommand.has_value());

    // Then the real confirming poll arrives.
    auto confirmedChange = rec.applyOperationalObservation(opPoll(true, OperatingMode::Cool, 2600));
    REQUIRE_FALSE(confirmedChange.sendCommand.has_value());
    REQUIRE(state.coolSetpoint.observed() == 2600);
    REQUIRE_FALSE(state.coolSetpoint.inFlight().has_value());
}

TEST_CASE("Integration: external panel change between confirmed write and next poll",
          "[phase7][integration]")
{
    ManualTimeSource time;
    LogicalACState   state(LogicalACStateDefaults{
        .onOff = true, .mode = OperationalMode::Cool, .coolSetpoint = 2400});
    Reconciler       rec(state, time);
    rec.applyOperationalObservation(opPoll(true, OperatingMode::Cool, 2400));

    // Matter write → confirm.
    auto change = rec.applyIntent(SetOccupiedCoolingSetpointIntent{2600});
    rec.onCommandSent(*change.sendCommand);
    rec.applyOperationalObservation(opPoll(true, OperatingMode::Cool, 2600));
    REQUIRE(state.coolSetpoint.attribution() == ObservationSource::Matter);

    // Panel pushes setpoint to 22. Walk past the guard window first so
    // the change is treated as an external override.
    time.advance(2'000);
    auto change2 = rec.applyOperationalObservation(opPoll(true, OperatingMode::Cool, 2200));
    REQUIRE(state.coolSetpoint.observed() == 2200);
    REQUIRE(state.coolSetpoint.attribution() == ObservationSource::Device);
    REQUIRE_FALSE(change2.sendCommand.has_value()); // we accept the panel value
}

TEST_CASE("Integration: bundled setpoint pair commits as one D1",
          "[phase7][integration][bundle]")
{
    // The reconciler primitive that future Matter AtomicRequest wiring
    // will invoke (see atomic-txn-completion-plan.md). Sequential edits
    // would walk the auto centre through an intermediate value; the
    // bundle locks it to midpoint(heat, cool).
    ManualTimeSource time;
    LogicalACState   state(LogicalACStateDefaults{
        .onOff = true, .mode = OperationalMode::Auto, .autoSetpoint = 2200,
        .heatSetpoint = 2000, .coolSetpoint = 2500});
    Reconciler       rec(state, time);
    rec.applyOperationalObservation(opPoll(true, OperatingMode::Auto, 2200));

    auto change = rec.applyIntentBundle({
        SetOccupiedHeatingSetpointIntent{2350},
        SetOccupiedCoolingSetpointIntent{2450},
    });

    REQUIRE(change.sendCommand.has_value());
    REQUIRE(change.sendCommand->operatingMode  == OperatingMode::Auto);
    REQUIRE(change.sendCommand->setpointCelsius == 2400); // midpoint of (2350, 2450)
}
