/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Tests for sync::BridgeKernel — the data-ownership root. The kernel has
 * no locks of its own; this suite exercises the public surface and pins
 * the contract that SyncCoordinator's locking layer relies on.
 *
 * The reconciler/projector/atomic/diff behaviours have their own dedicated
 * suites (test_reconciler.cpp, test_projector.cpp, test_atomic_buffer.cpp).
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

// ─── Atomic surface ──────────────────────────────────────────────────────────

TEST_CASE("begin/atomicWrite/commit routes through AtomicTxn",
          "[bridge_kernel][atomic]")
{
    ManualTimeSource time;
    BridgeKernel k(time, ReconcilerConfig{});
    k.applyOperationalObservation({true, OperatingMode::Cool, 2400, FanMode::Auto, std::nullopt});

    REQUIRE(k.begin() == AtomicTxn::Status::Ok);
    REQUIRE(k.atomicWrite(SetOccupiedCoolingSetpointIntent{2600}) == AtomicTxn::Status::Ok);
    auto change = k.commit();
    REQUIRE(change.sendCommand.has_value());
    REQUIRE(change.sendCommand->setpointCelsius == 2600);
}

TEST_CASE("rollback discards the buffer", "[bridge_kernel][atomic]")
{
    ManualTimeSource time;
    BridgeKernel k(time, ReconcilerConfig{});
    k.applyOperationalObservation({true, OperatingMode::Cool, 2400, FanMode::Auto, std::nullopt});

    k.begin();
    k.atomicWrite(SetOccupiedCoolingSetpointIntent{2600});
    REQUIRE(k.rollback() == AtomicTxn::Status::Ok);

    // Buffer dropped; pendingCommand stays empty.
    REQUIRE_FALSE(k.pendingCommand().has_value());
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
    REQUIRE(k.readFanIsAuto() == true);
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
