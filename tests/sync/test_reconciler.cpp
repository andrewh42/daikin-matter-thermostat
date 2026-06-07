/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Tests for sync::Reconciler. Groups mirror the plan:
 *   A  Echo suppression / TOCTOU
 *   B  Auto mode (3-slot shadow)
 *   C  Coalescing
 *   D  Conflict resolution & provenance
 */

#include <catch2/catch_test_macros.hpp>

#include "reconciler.h"

#include <algorithm>

using namespace sync;

using SystemModeEnum            = chip::app::Clusters::Thermostat::SystemModeEnum;
using ThermostatRunningModeEnum = chip::app::Clusters::Thermostat::ThermostatRunningModeEnum;
namespace TAttr  = chip::app::Clusters::Thermostat::Attributes;
namespace FCAttr = chip::app::Clusters::FanControl::Attributes;
namespace OOAttr = chip::app::Clusters::OnOff::Attributes;

// ─── Test fixture & helpers ──────────────────────────────────────────────────

namespace {

struct Harness {
    ManualTimeSource time;
    LogicalACState   state;
    Reconciler       rec;

    Harness(LogicalACStateDefaults defaults = {}, ReconcilerConfig cfg = {})
        : time(0), state(defaults), rec(state, time, cfg)
    {
    }
};

bool containsAttr(const std::vector<MatterAttributePath>& paths,
                  chip::ClusterId cluster, chip::AttributeId attribute)
{
    return std::any_of(paths.begin(), paths.end(),
        [&](const MatterAttributePath& p) {
            return p.cluster == cluster && p.attribute == attribute;
        });
}

S21State pollState(bool onOff, OperatingMode mode, int16_t setpoint,
                   FanMode fan = FanMode::Auto,
                   int16_t indoor = 2300, int16_t outdoor = 1500,
                   uint8_t humidity = 50)
{
    return S21State{onOff, mode, setpoint, fan, indoor, outdoor, humidity};
}

LogicalACStateDefaults coolModeDefaults(int16_t coolSp = 2400)
{
    LogicalACStateDefaults d;
    d.onOff        = true;
    d.mode         = SystemModeEnum::kCool;
    d.coolSetpoint = coolSp;
    return d;
}

LogicalACStateDefaults autoModeDefaults(int16_t autoSp = 2200)
{
    LogicalACStateDefaults d;
    d.onOff        = true;
    d.mode         = SystemModeEnum::kAuto;
    d.autoSetpoint = autoSp;
    d.heatSetpoint = 2000;
    d.coolSetpoint = 2500;
    return d;
}

} // namespace

// ─── Group A — echo suppression / TOCTOU ─────────────────────────────────────

TEST_CASE("A1: stale poll does not revert a fresher controller write",
          "[phase2][reconciler][groupA]")
{
    Harness h(coolModeDefaults(2400));

    // Fresh device baseline so it's not "boot defaults".
    h.rec.applyObservation(pollState(true, OperatingMode::Cool, 2400));
    REQUIRE(h.state.coolSetpoint.observed() == 2400);

    // Controller writes a new desired.
    auto change = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2600});
    REQUIRE(h.state.coolSetpoint.desired() == 2600);
    REQUIRE_FALSE(h.state.coolSetpoint.inFlight().has_value());
    REQUIRE(change.sendCommand.has_value());
    REQUIRE(change.sendCommand->setpointCelsius == 2600);

    // Pump dispatches and reports it to the reconciler.
    h.rec.onCommandSent(*change.sendCommand);
    REQUIRE(h.state.coolSetpoint.inFlight().has_value());
    REQUIRE(*h.state.coolSetpoint.inFlight() == 2600);

    // Stale poll: still reading the pre-write setpoint of 2400. This must
    // NOT clobber desired or in-flight — TwinField's stale-poll branch
    // (value == observed while inFlight is set) leaves state unchanged.
    auto staleChange = h.rec.applyObservation(pollState(true, OperatingMode::Cool, 2400));

    REQUIRE(h.state.coolSetpoint.observed() == 2400);
    REQUIRE(h.state.coolSetpoint.inFlight().has_value());
    REQUIRE(*h.state.coolSetpoint.inFlight() == 2600);
    REQUIRE(h.state.coolSetpoint.desired()  == 2600);
    // No D1 emitted: pendingCommand would dedup against mLastSentCommand.
    REQUIRE_FALSE(staleChange.sendCommand.has_value());
    // No dirty marker for the cluster either — nothing changed externally.
    REQUIRE(staleChange.dirtyAttributes.empty());
}

TEST_CASE("A2: confirmation clears in-flight without re-emitting a command",
          "[phase2][reconciler][groupA]")
{
    Harness h(coolModeDefaults(2400));
    h.rec.applyObservation(pollState(true, OperatingMode::Cool, 2400));

    auto change = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2600});
    h.rec.onCommandSent(*change.sendCommand);

    // Confirming poll.
    auto next = h.rec.applyObservation(pollState(true, OperatingMode::Cool, 2600));

    REQUIRE(h.state.coolSetpoint.observed() == 2600);
    REQUIRE_FALSE(h.state.coolSetpoint.inFlight().has_value());
    REQUIRE_FALSE(h.state.coolSetpoint.dirty());
    REQUIRE_FALSE(next.sendCommand.has_value());
}

TEST_CASE("A3: external change while no in-flight pulls desired to device value",
          "[phase2][reconciler][groupA]")
{
    Harness h(coolModeDefaults(2400));
    h.rec.applyObservation(pollState(true, OperatingMode::Cool, 2400));

    auto change = h.rec.applyObservation(pollState(true, OperatingMode::Cool, 2200));

    REQUIRE(h.state.coolSetpoint.observed() == 2200);
    REQUIRE(h.state.coolSetpoint.desired()  == 2200);
    REQUIRE_FALSE(change.sendCommand.has_value());
    REQUIRE(containsAttr(change.dirtyAttributes,
                         chip::app::Clusters::Thermostat::Id,
                         TAttr::OccupiedCoolingSetpoint::Id));
}

TEST_CASE("A4: disconfirmation when device clamps", "[phase2][reconciler][groupA]")
{
    Harness h(coolModeDefaults(3000));
    h.rec.applyObservation(pollState(true, OperatingMode::Cool, 3000));

    auto change = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{3500});
    h.rec.onCommandSent(*change.sendCommand);

    // Device clamped to 3200 (≠ inFlight 3500, ≠ pre-send observed 3000).
    auto disconfirmed = h.rec.applyObservation(pollState(true, OperatingMode::Cool, 3200));

    REQUIRE(h.state.coolSetpoint.observed() == 3200);
    // Desired is preserved so any queued controller intent isn't wiped;
    // dedup against mLastSentCommand stops us from retrying the same value.
    REQUIRE(h.state.coolSetpoint.desired()  == 3500);
    REQUIRE_FALSE(h.state.coolSetpoint.inFlight().has_value());
    REQUIRE_FALSE(disconfirmed.sendCommand.has_value());
    REQUIRE(containsAttr(disconfirmed.dirtyAttributes,
                         chip::app::Clusters::Thermostat::Id,
                         TAttr::OccupiedCoolingSetpoint::Id));
}

TEST_CASE("A5: multi-attribute snapshot — fresh temp + stale setpoint",
          "[phase2][reconciler][groupA]")
{
    Harness h(coolModeDefaults(2400));
    h.rec.applyObservation(pollState(true, OperatingMode::Cool, 2400, FanMode::Auto, 2300));

    // Controller raises setpoint.
    auto change = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2600});
    h.rec.onCommandSent(*change.sendCommand);

    // Poll: setpoint still stale (2400), temperature fresh (2350).
    auto pollChange = h.rec.applyObservation(
        pollState(true, OperatingMode::Cool, 2400, FanMode::Auto, 2350));

    REQUIRE(h.state.indoorTemp.observed() == 2350);
    REQUIRE(containsAttr(pollChange.dirtyAttributes,
                         chip::app::Clusters::Thermostat::Id,
                         TAttr::LocalTemperature::Id));
}

// ─── Group B — Auto-mode 3-slot shadow ───────────────────────────────────────

TEST_CASE("B6: auto-target persists across mode flip Cool→Auto→Cool",
          "[phase2][reconciler][groupB]")
{
    LogicalACStateDefaults d;
    d.onOff        = true;
    d.mode         = SystemModeEnum::kCool;
    d.heatSetpoint = 2000;
    d.coolSetpoint = 2400;
    d.autoSetpoint = 2200;
    Harness h(d);

    auto toAuto = h.rec.applyIntent(SetSystemModeIntent{SystemModeEnum::kAuto});
    REQUIRE(toAuto.sendCommand.has_value());
    REQUIRE(toAuto.sendCommand->operatingMode  == OperatingMode::Auto);
    REQUIRE(toAuto.sendCommand->setpointCelsius == 2200); // unchanged auto shadow
    h.rec.onCommandSent(*toAuto.sendCommand);

    auto backToCool = h.rec.applyIntent(SetSystemModeIntent{SystemModeEnum::kCool});
    REQUIRE(backToCool.sendCommand.has_value());
    REQUIRE(backToCool.sendCommand->operatingMode  == OperatingMode::Cool);
    REQUIRE(backToCool.sendCommand->setpointCelsius == 2400);
    REQUIRE(h.state.autoSetpoint.desired() == 2200); // shadow intact
}

TEST_CASE("B7: heat-edge edit in Auto recomputes centre as midpoint",
          "[phase2][reconciler][groupB]")
{
    Harness h(autoModeDefaults(2200));
    h.rec.applyObservation(pollState(true, OperatingMode::Auto, 2200));

    // Projected band before: cool=22.5, heat=21.5
    // Controller drags heat edge down to 19.0
    // New centre = (22.5 + 19.0) / 2 = 20.75  → 2075 in 0.01°C
    auto change = h.rec.applyIntent(SetOccupiedHeatingSetpointIntent{1900});

    REQUIRE(h.state.autoSetpoint.desired() == 2075);
    REQUIRE(change.sendCommand.has_value());
    REQUIRE(change.sendCommand->operatingMode  == OperatingMode::Auto);
    REQUIRE(change.sendCommand->setpointCelsius == 2075);
}

TEST_CASE("B8: cool-edge edit in Auto is symmetric to heat-edge edit",
          "[phase2][reconciler][groupB]")
{
    Harness h(autoModeDefaults(2200));
    h.rec.applyObservation(pollState(true, OperatingMode::Auto, 2200));

    // Drag cool edge up to 25.0.
    // New centre = (2500 + (2200-50)) / 2 = (2500 + 2150)/2 = 2325
    auto change = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2500});

    REQUIRE(h.state.autoSetpoint.desired() == 2325);
    REQUIRE(change.sendCommand->setpointCelsius == 2325);
}

TEST_CASE("B9: band-translate (both edges +2°C in two intents) → centre +2°C",
          "[phase2][reconciler][groupB]")
{
    Harness h(autoModeDefaults(2200));
    h.rec.applyObservation(pollState(true, OperatingMode::Auto, 2200));

    // Heat edge: 21.5 → 23.5  (centre becomes (22.5 + 23.5)/2 = 23.0)
    h.rec.applyIntent(SetOccupiedHeatingSetpointIntent{2350});
    REQUIRE(h.state.autoSetpoint.desired() == 2300);

    // Cool edge: 22.5 → 24.5
    // Projected heat from current desired = 2300 - 50 = 2250
    // New centre = (2450 + 2250) / 2 = 2350
    h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2450});
    REQUIRE(h.state.autoSetpoint.desired() == 2350);

    // Net centre +1.5°C from a +2°C move on each edge. The midpoint rule
    // doesn't recover the exact band-translate in two non-atomic intents
    // because the heat edit shifts the centre, which then biases the cool
    // edit. Group C/Phase 4 (atomic batch) handles the exact "both edges
    // together" case; this test pins the two-step approximation.
}

TEST_CASE("B9b: atomic-style simultaneous edge moves → exact centre translation",
          "[phase2][reconciler][groupB]")
{
    Harness h(autoModeDefaults(2200));
    h.rec.applyObservation(pollState(true, OperatingMode::Auto, 2200));

    // Simulate Phase 4's atomic flush: snapshot before, then apply both
    // edges relative to the pre-flush projection. We mimic that here by
    // computing both new desireds using the SAME starting state — i.e.
    // suppressing the centre drift between intents. The reconciler doesn't
    // yet model this; the test documents the gap that Phase 4 closes.
    SUCCEED("Documented: exact band-translate semantics arrive with Phase 4 atomic buffer.");
}

// ─── Group C — coalescing ────────────────────────────────────────────────────

TEST_CASE("C10: two intents before send produce one composite command",
          "[phase2][reconciler][groupC]")
{
    Harness h(coolModeDefaults(2400));
    h.rec.applyObservation(pollState(true, OperatingMode::Cool, 2400));

    h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2600});
    auto change = h.rec.applyIntent(SetSystemModeIntent{SystemModeEnum::kHeat});

    REQUIRE(change.sendCommand.has_value());
    REQUIRE(change.sendCommand->operatingMode  == OperatingMode::Heat);
    // Heat mode shadow defaults to LogicalACStateDefaults.heatSetpoint=2000.
    REQUIRE(change.sendCommand->setpointCelsius == 2000);
}

TEST_CASE("C11: intent equal to current desired is a no-op",
          "[phase2][reconciler][groupC]")
{
    Harness h(coolModeDefaults(2400));
    h.rec.applyObservation(pollState(true, OperatingMode::Cool, 2400));

    auto change = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2400});

    REQUIRE_FALSE(change.sendCommand.has_value());
    REQUIRE(change.dirtyAttributes.empty());
}

TEST_CASE("Cdedup: identical recomputed command is suppressed",
          "[phase2][reconciler][groupC]")
{
    Harness h(coolModeDefaults(2400));
    h.rec.applyObservation(pollState(true, OperatingMode::Cool, 2400));

    auto change1 = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2600});
    REQUIRE(change1.sendCommand.has_value());
    h.rec.onCommandSent(*change1.sendCommand);
    // Confirm so dirty clears.
    h.rec.applyObservation(pollState(true, OperatingMode::Cool, 2600));

    // Re-applying the same desired again must not re-emit.
    auto change2 = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2600});
    REQUIRE_FALSE(change2.sendCommand.has_value());
}

// ─── Group D — conflict resolution & provenance ──────────────────────────────

TEST_CASE("D13: Matter intent inside guard window is dropped after device change",
          "[phase2][reconciler][groupD]")
{
    Harness h(coolModeDefaults(2400));
    h.time.set(10'000);
    h.rec.applyObservation(pollState(true, OperatingMode::Cool, 2400));

    // Device-side change at t=10000ms.
    h.time.advance(5'000);
    h.rec.applyObservation(pollState(true, OperatingMode::Cool, 2200));
    REQUIRE(h.state.coolSetpoint.observed() == 2200);

    // Matter intent arrives 100ms later with an *older* value — within
    // the 1000ms guard window.
    h.time.advance(100);
    auto change = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2400});

    REQUIRE(h.state.coolSetpoint.desired()  == 2200); // intent dropped
    REQUIRE_FALSE(change.sendCommand.has_value());
}

TEST_CASE("D14: Matter intent outside guard window wins",
          "[phase2][reconciler][groupD]")
{
    Harness h(coolModeDefaults(2400));
    h.time.set(10'000);
    h.rec.applyObservation(pollState(true, OperatingMode::Cool, 2200));

    h.time.advance(2'000); // > 1000ms guard
    auto change = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2400});

    REQUIRE(h.state.coolSetpoint.desired() == 2400);
    REQUIRE(change.sendCommand.has_value());
    REQUIRE(change.sendCommand->setpointCelsius == 2400);
}

TEST_CASE("D14b: guard window does not block intent matching current observed",
          "[phase2][reconciler][groupD]")
{
    Harness h(coolModeDefaults(2400));
    h.time.set(10'000);
    h.rec.applyObservation(pollState(true, OperatingMode::Cool, 2200));

    // Intent inside guard window but value matches observed — nothing to
    // conflict over, must be applied.
    h.time.advance(100);
    auto change = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2200});

    REQUIRE(h.state.coolSetpoint.desired() == 2200);
    REQUIRE_FALSE(change.sendCommand.has_value()); // already-matched → no D1
}

// ─── Phase 9 — SetpointChangeSource attribution ──────────────────────────────

TEST_CASE("D15a: confirmed Matter intent attributes to External",
          "[phase9][reconciler][groupD]")
{
    Harness h(coolModeDefaults(2400));
    h.rec.applyObservation(pollState(true, OperatingMode::Cool, 2400));
    REQUIRE(h.state.coolSetpoint.attribution() == ObservationSource::Device);

    auto change = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2600});
    REQUIRE(h.state.coolSetpoint.attribution() == ObservationSource::Matter);

    h.rec.onCommandSent(*change.sendCommand);
    // Confirming poll: attribution stays Matter (controller "wins" credit).
    h.rec.applyObservation(pollState(true, OperatingMode::Cool, 2600));
    REQUIRE(h.state.coolSetpoint.attribution() == ObservationSource::Matter);
}

TEST_CASE("D15b: external panel change flips attribution to Device",
          "[phase9][reconciler][groupD]")
{
    Harness h(coolModeDefaults(2400));
    h.rec.applyObservation(pollState(true, OperatingMode::Cool, 2400));

    // Pretend Matter wrote a value first, was confirmed, then physical
    // panel overrode it.
    auto change = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2500});
    h.rec.onCommandSent(*change.sendCommand);
    h.rec.applyObservation(pollState(true, OperatingMode::Cool, 2500));
    REQUIRE(h.state.coolSetpoint.attribution() == ObservationSource::Matter);

    // Physical panel changes to 2200 — wins guard window because we're
    // past it (time hasn't advanced — but no in-flight is set, so the
    // change is treated as an external observation regardless).
    h.time.advance(2'000);
    h.rec.applyObservation(pollState(true, OperatingMode::Cool, 2200));
    REQUIRE(h.state.coolSetpoint.attribution() == ObservationSource::Device);
}

TEST_CASE("D15c: disconfirmation flips attribution to Device",
          "[phase9][reconciler][groupD]")
{
    Harness h(coolModeDefaults(3000));
    h.rec.applyObservation(pollState(true, OperatingMode::Cool, 3000));

    auto change = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{3500});
    h.rec.onCommandSent(*change.sendCommand);

    // Device clamps — disconfirmation. Attribution should reflect that
    // the device, not the controller, is now responsible for the value.
    h.rec.applyObservation(pollState(true, OperatingMode::Cool, 3200));
    REQUIRE(h.state.coolSetpoint.attribution() == ObservationSource::Device);
}
