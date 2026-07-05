/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Tests for sync::BridgeKernel — the data-ownership root. The kernel has
 * no locks of its own; this suite exercises the public surface and pins
 * the contract that SyncCoordinator's locking layer relies on.
 *
 * The reconciler/projector/bundle/diff behaviours have their own dedicated
 * suites (test_reconciler.cpp, test_projector.cpp, test_intent_bundle.cpp).
 * Tests here focus on the kernel as the composition boundary: every public
 * method routes correctly and returns the expected change type.
 */

#include <catch2/catch_test_macros.hpp>

#include "bridge_kernel.h"
#include "logical_attribute.h"

#include <algorithm>

using namespace sync;

namespace {

LogicalACStateDefaults coolDefaults(int16_t coolSp = 2400)
{
    LogicalACStateDefaults d;
    d.onOff        = true;
    d.mode         = OperationalMode::Cool;
    d.coolSetpoint = coolSp;
    return d;
}

bool contains(const std::vector<LogicalAttribute>& attrs, LogicalAttribute target)
{
    return std::any_of(attrs.begin(), attrs.end(),
        [&](LogicalAttribute a) { return a == target; });
}

} // namespace

TEST_CASE("Kernel default-constructs with sensible defaults",
          "[bridge_kernel]")
{
    ManualTimeSource time;
    BridgeKernel k(time);
    REQUIRE(k.readOnOff() == false);
    REQUIRE(k.readMode()  == OperationalMode::Auto);
    REQUIRE(k.readRunningMode() == RunningMode::Off);
    REQUIRE_FALSE(k.readLocalTemperature().has_value());
    REQUIRE_FALSE(k.readReachable());
}

TEST_CASE("applyIntent produces a sendCommand and dirty attributes",
          "[bridge_kernel]")
{
    ManualTimeSource time;
    BridgeKernel k(time);
    // Bring the kernel into a known op-state before applying an intent so
    // the observation hardens the device baseline.
    k.applyOperationalObservation({true, OperatingMode::Cool, 2400, FanMode::Auto, std::nullopt});

    auto change = k.applyIntent(SetOccupiedCoolingSetpointIntent{2600});
    REQUIRE(change.sendCommand.has_value());
    REQUIRE(change.sendCommand->setpointCelsius == 2600);
    REQUIRE(contains(change.dirtyAttributes, LogicalAttribute::OccupiedCoolingSetpoint));
}

TEST_CASE("applyOperationalObservation populates state.runningMode via fusion",
          "[bridge_kernel]")
{
    ManualTimeSource time;
    BridgeKernel k(time);
    k.applyOperationalObservation({true, OperatingMode::Auto_Cooling, 2200, FanMode::Auto, std::nullopt});
    REQUIRE(k.readRunningMode() == RunningMode::Cooling);
}

TEST_CASE("applyEnvironmentalObservation returns EnvironmentalChange (no sendCommand slot)",
          "[bridge_kernel]")
{
    ManualTimeSource time;
    BridgeKernel k(time);
    EnvironmentalChange env = k.applyEnvironmentalObservation({2300, 1500, 50});
    // Static guarantee: EnvironmentalChange has no sendCommand. The
    // assignment to a typed local pins that the kernel returns the right
    // shape. The dirty-attribute content itself is covered by reconciler tests.
    (void)env;
    SUCCEED("kernel.applyEnvironmentalObservation returned EnvironmentalChange");
}

TEST_CASE("onCommandSent dedups pendingCommand against the same value",
          "[bridge_kernel]")
{
    ManualTimeSource time;
    BridgeKernel k(time);
    k.applyOperationalObservation({true, OperatingMode::Cool, 2400, FanMode::Auto, std::nullopt});

    auto change = k.applyIntent(SetOccupiedCoolingSetpointIntent{2600});
    REQUIRE(change.sendCommand.has_value());

    k.onCommandSent(*change.sendCommand);
    // After onCommandSent: inFlight=2600 promoted from desired, mLastSentCommand
    // remembers the value, so pendingCommand returns nullopt (no work to do).
    REQUIRE_FALSE(k.pendingCommand().has_value());
}

TEST_CASE("onCommandFailed clears mLastSentCommand so a disconfirming poll can re-emit",
          "[bridge_kernel]")
{
    // The dedup check inside pendingCommand suppresses a re-emit of the
    // same command. onCommandFailed clears that memo so a follow-up
    // disconfirming poll (which clears inFlight) can re-emit the same
    // command in a retry.
    ManualTimeSource time;
    BridgeKernel k(time);
    k.applyOperationalObservation({true, OperatingMode::Cool, 2400, FanMode::Auto, std::nullopt});

    auto change = k.applyIntent(SetOccupiedCoolingSetpointIntent{2600});
    k.onCommandSent(*change.sendCommand);

    k.onCommandFailed();

    // Disconfirming poll: value 2500 ≠ inFlight (2600) and ≠ observed
    // (2400) → TwinField clears inFlight, sets observed=2500. desired is
    // left at 2600, so the twin is dirty again.
    auto retry = k.applyOperationalObservation(
        {true, OperatingMode::Cool, 2500, FanMode::Auto, std::nullopt});
    REQUIRE(retry.sendCommand.has_value());
    REQUIRE(retry.sendCommand->setpointCelsius == 2600);
}

TEST_CASE("notifyLinkDown flips reachable to false, observation restores it",
          "[bridge_kernel]")
{
    ManualTimeSource time;
    BridgeKernel k(time);
    k.applyOperationalObservation({true, OperatingMode::Cool, 2400, FanMode::Auto, std::nullopt});
    REQUIRE(k.readReachable());

    k.notifyLinkDown();
    REQUIRE_FALSE(k.readReachable());

    k.applyOperationalObservation({true, OperatingMode::Cool, 2400, FanMode::Auto, std::nullopt});
    REQUIRE(k.readReachable());
}

// ─── Read surface ────────────────────────────────────────────────────────────

TEST_CASE("Per-attribute reads route to the projector and reflect mutations",
          "[bridge_kernel]")
{
    ManualTimeSource time;
    BridgeKernel k(time);
    k.applyOperationalObservation({true, OperatingMode::Cool, 2400, FanMode::Auto, std::nullopt});
    k.applyEnvironmentalObservation({2350, 1500, 55});

    REQUIRE(k.readOnOff() == true);
    REQUIRE(k.readMode()  == OperationalMode::Cool);
    REQUIRE(k.readLocalTemperature().has_value());
    REQUIRE(*k.readLocalTemperature() == 2350);
    REQUIRE(k.readHumidityCentiPercent().has_value());
    REQUIRE(*k.readHumidityCentiPercent() == 5500);
    REQUIRE(k.readFanMode() == FanModeCategory::Auto);
}

TEST_CASE("Snapshot returns a value-copy of LogicalACState",
          "[bridge_kernel]")
{
    ManualTimeSource time;
    BridgeKernel k(time);
    k.applyOperationalObservation({true, OperatingMode::Cool, 2400, FanMode::Auto, std::nullopt});

    LogicalACState s = k.snapshot();
    REQUIRE(s.onOff.observed() == true);
    REQUIRE(s.mode.observed()  == OperationalMode::Cool);
    REQUIRE(s.coolSetpoint.observed() == 2400);
}

// ─── projectionSnapshot ──────────────────────────────────────────────────────

namespace {

/// Field-by-field equality on the projection. Kept local to this suite so a
/// new ProjectedClusterState field forces an explicit update here (the
/// compiler won't, but the omission shows up as an obvious gap).
bool projectionsEqual(const ProjectedClusterState& a, const ProjectedClusterState& b)
{
    return a.onOff                   == b.onOff
        && a.mode                    == b.mode
        && a.occupiedHeatingSetpoint == b.occupiedHeatingSetpoint
        && a.occupiedCoolingSetpoint == b.occupiedCoolingSetpoint
        && a.runningMode             == b.runningMode
        && a.localTemperature        == b.localTemperature
        && a.outdoorTemperature      == b.outdoorTemperature
        && a.setpointSource          == b.setpointSource
        && a.speedSetting            == b.speedSetting
        && a.fanMode                 == b.fanMode
        && a.speedCurrent            == b.speedCurrent
        && a.percentSetting          == b.percentSetting
        && a.percentCurrent          == b.percentCurrent
        && a.humidityCentiPercent    == b.humidityCentiPercent
        && a.reachable               == b.reachable;
}

} // namespace

TEST_CASE("projectionSnapshot is a pure delegate to projector().project(state)",
          "[bridge_kernel]")
{
    ManualTimeSource time;
    BridgeKernel k(time);
    k.applyOperationalObservation({true, OperatingMode::Cool, 2400, FanMode::Auto, std::nullopt});
    k.applyEnvironmentalObservation({2350, 1500, 55});
    k.applyIntent(SetOccupiedCoolingSetpointIntent{2600});

    // The snapshot must equal what the kernel's own projector would produce
    // directly off the raw state — proving projectionSnapshot adds no policy.
    REQUIRE(projectionsEqual(k.projectionSnapshot(),
                             k.projector().project(k.snapshot())));
}

TEST_CASE("projectionSnapshot reflects post-intent / post-observation state",
          "[bridge_kernel]")
{
    ManualTimeSource time;
    BridgeKernel k(time);

    // Default-constructed kernel projects power off.
    REQUIRE_FALSE(k.projectionSnapshot().onOff);

    // After an on+Cool observation the same one-shot read sees both fields
    // move together — the coherence this method exists to guarantee.
    k.applyOperationalObservation({true, OperatingMode::Cool, 2400, FanMode::Auto, std::nullopt});
    const auto p = k.projectionSnapshot();
    REQUIRE(p.onOff);
    REQUIRE(p.mode == OperationalMode::Cool);
}
