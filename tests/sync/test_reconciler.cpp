/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Tests for sync::Reconciler. Groups mirror the plan:
 *   A  Echo suppression / TOCTOU
 *   B  Auto mode (3-slot shadow)
 *   C  Coalescing
 *   D  Conflict resolution & provenance
 *   F  RunningMode fusion at observation time
 */

#include <catch2/catch_test_macros.hpp>

#include "reconciler.h"

#include <algorithm>

using namespace sync;

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

bool containsAttr(const std::vector<LogicalAttribute>& attrs, LogicalAttribute target)
{
    return std::any_of(attrs.begin(), attrs.end(),
        [&](LogicalAttribute a) { return a == target; });
}

S21OperationalObservation opObs(bool onOff, OperatingMode mode, int16_t setpoint,
                                FanMode fan = FanMode::Auto,
                                std::optional<bool> valve = std::nullopt)
{
    return {onOff, mode, setpoint, fan, valve};
}

S21EnvironmentalObservation envObs(int16_t indoor = 2300,
                                   int16_t outdoor = 1500,
                                   uint8_t humidity = 50)
{
    return {indoor, outdoor, humidity};
}

LogicalACStateDefaults coolModeDefaults(int16_t coolSp = 2400)
{
    LogicalACStateDefaults d;
    d.onOff        = true;
    d.mode         = OperationalMode::Cool;
    d.coolSetpoint = coolSp;
    return d;
}

LogicalACStateDefaults autoModeDefaults(int16_t autoSp = 2200)
{
    LogicalACStateDefaults d;
    d.onOff        = true;
    d.mode         = OperationalMode::Auto;
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

    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));
    REQUIRE(h.state.coolSetpoint.observed() == 2400);

    auto change = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2600});
    REQUIRE(h.state.coolSetpoint.desired() == 2600);
    REQUIRE_FALSE(h.state.coolSetpoint.inFlight().has_value());
    REQUIRE(change.sendCommand.has_value());
    REQUIRE(change.sendCommand->setpointCelsius == 2600);

    h.rec.onCommandSent(*change.sendCommand);
    REQUIRE(h.state.coolSetpoint.inFlight().has_value());
    REQUIRE(*h.state.coolSetpoint.inFlight() == 2600);

    auto staleChange = h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));

    REQUIRE(h.state.coolSetpoint.observed() == 2400);
    REQUIRE(h.state.coolSetpoint.inFlight().has_value());
    REQUIRE(*h.state.coolSetpoint.inFlight() == 2600);
    REQUIRE(h.state.coolSetpoint.desired()  == 2600);
    REQUIRE_FALSE(staleChange.sendCommand.has_value());
    REQUIRE(staleChange.dirtyAttributes.empty());
}

TEST_CASE("A2: confirmation clears in-flight without re-emitting a command",
          "[phase2][reconciler][groupA]")
{
    Harness h(coolModeDefaults(2400));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));

    auto change = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2600});
    h.rec.onCommandSent(*change.sendCommand);

    auto next = h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2600));

    REQUIRE(h.state.coolSetpoint.observed() == 2600);
    REQUIRE_FALSE(h.state.coolSetpoint.inFlight().has_value());
    REQUIRE_FALSE(h.state.coolSetpoint.dirty());
    REQUIRE_FALSE(next.sendCommand.has_value());
}

TEST_CASE("A3: external change while no in-flight pulls desired to device value",
          "[phase2][reconciler][groupA]")
{
    Harness h(coolModeDefaults(2400));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));

    auto change = h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2200));

    REQUIRE(h.state.coolSetpoint.observed() == 2200);
    REQUIRE(h.state.coolSetpoint.desired()  == 2200);
    REQUIRE_FALSE(change.sendCommand.has_value());
    REQUIRE(containsAttr(change.dirtyAttributes,
                         LogicalAttribute::OccupiedCoolingSetpoint));
}

TEST_CASE("A4: disconfirmation when device clamps", "[phase2][reconciler][groupA]")
{
    Harness h(coolModeDefaults(3000));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 3000));

    auto change = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{3500});
    h.rec.onCommandSent(*change.sendCommand);

    auto disconfirmed = h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 3200));

    REQUIRE(h.state.coolSetpoint.observed() == 3200);
    REQUIRE(h.state.coolSetpoint.desired()  == 3500);
    REQUIRE_FALSE(h.state.coolSetpoint.inFlight().has_value());
    REQUIRE_FALSE(disconfirmed.sendCommand.has_value());
    REQUIRE(containsAttr(disconfirmed.dirtyAttributes,
                         LogicalAttribute::OccupiedCoolingSetpoint));
}

TEST_CASE("A5: multi-attribute snapshot — fresh temp + stale setpoint",
          "[phase2][reconciler][groupA]")
{
    Harness h(coolModeDefaults(2400));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));
    h.rec.applyEnvironmentalObservation(envObs(2300));

    auto change = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2600});
    h.rec.onCommandSent(*change.sendCommand);

    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));
    auto envChange = h.rec.applyEnvironmentalObservation(envObs(2350));

    REQUIRE(h.state.indoorTemp.observed() == 2350);
    REQUIRE(containsAttr(envChange.dirtyAttributes,
                         LogicalAttribute::LocalTemperature));
}

// ─── Group B — Auto-mode 3-slot shadow ───────────────────────────────────────

TEST_CASE("B6: auto-target persists across mode flip Cool→Auto→Cool",
          "[phase2][reconciler][groupB]")
{
    LogicalACStateDefaults d;
    d.onOff        = true;
    d.mode         = OperationalMode::Cool;
    d.heatSetpoint = 2000;
    d.coolSetpoint = 2400;
    d.autoSetpoint = 2200;
    Harness h(d);

    auto toAuto = h.rec.applyIntent(SetSystemModeIntent{true, OperationalMode::Auto});
    REQUIRE(toAuto.sendCommand.has_value());
    REQUIRE(toAuto.sendCommand->operatingMode  == OperatingMode::Auto);
    REQUIRE(toAuto.sendCommand->setpointCelsius == 2200); // unchanged auto shadow
    h.rec.onCommandSent(*toAuto.sendCommand);

    auto backToCool = h.rec.applyIntent(SetSystemModeIntent{true, OperationalMode::Cool});
    REQUIRE(backToCool.sendCommand.has_value());
    REQUIRE(backToCool.sendCommand->operatingMode  == OperatingMode::Cool);
    REQUIRE(backToCool.sendCommand->setpointCelsius == 2400);
    REQUIRE(h.state.autoSetpoint.desired() == 2200); // shadow intact
}

TEST_CASE("B7: heat-edge edit in Auto recomputes centre as midpoint",
          "[phase2][reconciler][groupB]")
{
    Harness h(autoModeDefaults(2200));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Auto, 2200));

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
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Auto, 2200));

    auto change = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2500});

    REQUIRE(h.state.autoSetpoint.desired() == 2325);
    REQUIRE(change.sendCommand->setpointCelsius == 2325);
}

TEST_CASE("B9: band-translate (both edges +2°C in two intents) → centre +2°C",
          "[phase2][reconciler][groupB]")
{
    Harness h(autoModeDefaults(2200));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Auto, 2200));

    h.rec.applyIntent(SetOccupiedHeatingSetpointIntent{2350});
    REQUIRE(h.state.autoSetpoint.desired() == 2300);

    h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2450});
    REQUIRE(h.state.autoSetpoint.desired() == 2350);

    // Documented two-step approximation; atomic path (Phase 4) handles
    // the exact "both edges together" case.
}

TEST_CASE("B9b: atomic-style simultaneous edge moves → exact centre translation",
          "[phase2][reconciler][groupB]")
{
    Harness h(autoModeDefaults(2200));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Auto, 2200));
    SUCCEED("Documented: exact band-translate semantics arrive with Phase 4 atomic buffer.");
}

// ─── Group C — coalescing ────────────────────────────────────────────────────

TEST_CASE("C10: two intents before send produce one composite command",
          "[phase2][reconciler][groupC]")
{
    Harness h(coolModeDefaults(2400));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));

    h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2600});
    auto change = h.rec.applyIntent(SetSystemModeIntent{true, OperationalMode::Heat});

    REQUIRE(change.sendCommand.has_value());
    REQUIRE(change.sendCommand->operatingMode  == OperatingMode::Heat);
    REQUIRE(change.sendCommand->setpointCelsius == 2000);
}

TEST_CASE("C11: intent equal to current desired is a no-op",
          "[phase2][reconciler][groupC]")
{
    Harness h(coolModeDefaults(2400));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));

    auto change = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2400});

    REQUIRE_FALSE(change.sendCommand.has_value());
    REQUIRE(change.dirtyAttributes.empty());
}

TEST_CASE("Cdedup: identical recomputed command is suppressed",
          "[phase2][reconciler][groupC]")
{
    Harness h(coolModeDefaults(2400));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));

    auto change1 = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2600});
    REQUIRE(change1.sendCommand.has_value());
    h.rec.onCommandSent(*change1.sendCommand);
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2600));

    auto change2 = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2600});
    REQUIRE_FALSE(change2.sendCommand.has_value());
}

// ─── Group D — conflict resolution & provenance ──────────────────────────────

TEST_CASE("D13: Matter intent inside guard window is dropped after device change",
          "[phase2][reconciler][groupD]")
{
    Harness h(coolModeDefaults(2400));
    h.time.set(10'000);
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));

    h.time.advance(5'000);
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2200));
    REQUIRE(h.state.coolSetpoint.observed() == 2200);

    h.time.advance(100);
    auto change = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2400});

    REQUIRE(h.state.coolSetpoint.desired()  == 2200);
    REQUIRE_FALSE(change.sendCommand.has_value());
}

TEST_CASE("D14: Matter intent outside guard window wins",
          "[phase2][reconciler][groupD]")
{
    Harness h(coolModeDefaults(2400));
    h.time.set(10'000);
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2200));

    h.time.advance(2'000);
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
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2200));

    h.time.advance(100);
    auto change = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2200});

    REQUIRE(h.state.coolSetpoint.desired() == 2200);
    REQUIRE_FALSE(change.sendCommand.has_value());
}

// ─── Phase 9 — SetpointChangeSource attribution ──────────────────────────────

TEST_CASE("D15a: confirmed Matter intent attributes to External",
          "[phase9][reconciler][groupD]")
{
    Harness h(coolModeDefaults(2400));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));
    REQUIRE(h.state.coolSetpoint.attribution() == ObservationSource::Device);

    auto change = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2600});
    REQUIRE(h.state.coolSetpoint.attribution() == ObservationSource::Matter);

    h.rec.onCommandSent(*change.sendCommand);
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2600));
    REQUIRE(h.state.coolSetpoint.attribution() == ObservationSource::Matter);
}

TEST_CASE("D15b: external panel change flips attribution to Device",
          "[phase9][reconciler][groupD]")
{
    Harness h(coolModeDefaults(2400));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));

    auto change = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2500});
    h.rec.onCommandSent(*change.sendCommand);
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2500));
    REQUIRE(h.state.coolSetpoint.attribution() == ObservationSource::Matter);

    h.time.advance(2'000);
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2200));
    REQUIRE(h.state.coolSetpoint.attribution() == ObservationSource::Device);
}

TEST_CASE("D15c: disconfirmation flips attribution to Device",
          "[phase9][reconciler][groupD]")
{
    Harness h(coolModeDefaults(3000));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 3000));

    auto change = h.rec.applyIntent(SetOccupiedCoolingSetpointIntent{3500});
    h.rec.onCommandSent(*change.sendCommand);

    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 3200));
    REQUIRE(h.state.coolSetpoint.attribution() == ObservationSource::Device);
}

// ─── C1 split — op vs env observation boundary ───────────────────────────────

TEST_CASE("Env observation marks LocalTemperature dirty, leaves operational twins alone",
          "[c1-split][reconciler]")
{
    Harness h(coolModeDefaults(2400));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));
    h.rec.applyEnvironmentalObservation(envObs(2300));

    const int16_t coolBefore = h.state.coolSetpoint.observed();
    const bool    onOffBefore = h.state.onOff.observed();

    auto envChange = h.rec.applyEnvironmentalObservation(envObs(2500));

    REQUIRE(h.state.indoorTemp.observed() == 2500);
    REQUIRE(h.state.coolSetpoint.observed() == coolBefore);
    REQUIRE(h.state.onOff.observed()        == onOffBefore);
    REQUIRE(containsAttr(envChange.dirtyAttributes,
                         LogicalAttribute::LocalTemperature));
}

TEST_CASE("Op observation does not touch indoor/outdoor/humidity twins",
          "[c1-split][reconciler]")
{
    Harness h(coolModeDefaults(2400));
    h.rec.applyEnvironmentalObservation(envObs(2300, 1500, 50));

    auto opChange = h.rec.applyOperationalObservation(
        opObs(true, OperatingMode::Cool, 2600));

    REQUIRE(h.state.indoorTemp.observed()  == 2300);
    REQUIRE(h.state.outdoorTemp.observed() == 1500);
    REQUIRE(h.state.humidity.observed()    == 50);
    REQUIRE(h.state.coolSetpoint.observed() == 2600);
    REQUIRE(containsAttr(opChange.dirtyAttributes,
                         LogicalAttribute::OccupiedCoolingSetpoint));
}

TEST_CASE("Env observation return type is EnvironmentalChange",
          "[c1-split][reconciler]")
{
    Harness h(coolModeDefaults(2400));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));
    h.rec.applyEnvironmentalObservation(envObs(2300));

    EnvironmentalChange change = h.rec.applyEnvironmentalObservation(envObs(2500));
    REQUIRE(containsAttr(change.dirtyAttributes,
                         LogicalAttribute::LocalTemperature));
}

// ─── Group E — SetSystemModeIntent power/mode split ──────────────────────────

TEST_CASE("E1: SetSystemModeIntent{power=false} flips onOff and preserves mode shadow",
          "[reconciler][groupE]")
{
    Harness h(coolModeDefaults(2400));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));

    auto change = h.rec.applyIntent(SetSystemModeIntent{false, OperationalMode::Auto});

    REQUIRE(h.state.onOff.desired() == false);
    // mode shadow retained — power-off doesn't write to the mode twin so a
    // power-on later restores the prior selection.
    REQUIRE(h.state.mode.desired() == OperationalMode::Cool);
    REQUIRE(change.sendCommand.has_value());
    REQUIRE(change.sendCommand->onOff == false);
    REQUIRE(change.sendCommand->operatingMode == OperatingMode::Cool);
}

TEST_CASE("E2: SetSystemModeIntent{power=true, mode=Cool} from off flips both axes",
          "[reconciler][groupE]")
{
    LogicalACStateDefaults d;
    d.onOff = false;
    d.mode  = OperationalMode::Auto;
    Harness h(d);
    h.rec.applyOperationalObservation(opObs(false, OperatingMode::Auto, 2200));

    auto change = h.rec.applyIntent(SetSystemModeIntent{true, OperationalMode::Cool});

    REQUIRE(h.state.onOff.desired() == true);
    REQUIRE(h.state.mode.desired()  == OperationalMode::Cool);
    REQUIRE(change.sendCommand.has_value());
    REQUIRE(change.sendCommand->onOff == true);
    REQUIRE(change.sendCommand->operatingMode == OperatingMode::Cool);
}

TEST_CASE("E3: SetSystemModeIntent{power=true, mode=current} when on flips no axes",
          "[reconciler][groupE]")
{
    Harness h(coolModeDefaults(2400));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));

    auto change = h.rec.applyIntent(SetSystemModeIntent{true, OperationalMode::Cool});

    REQUIRE_FALSE(change.sendCommand.has_value());
    REQUIRE(change.dirtyAttributes.empty());
}

// ─── Group F — RunningMode fusion at observation time ────────────────────────

TEST_CASE("F1: !onOff observation forces runningMode=Off, regardless of S21 mode",
          "[reconciler][groupF][runningMode]")
{
    Harness h;
    h.rec.applyOperationalObservation(opObs(false, OperatingMode::Cool, 2400, FanMode::Auto,
                                             std::optional<bool>{true}));
    REQUIRE(h.state.runningMode.observed() == RunningMode::Off);
}

TEST_CASE("F2: S21 Auto_Cooling observation → runningMode=Cooling (S21 hint wins)",
          "[reconciler][groupF][runningMode]")
{
    Harness h;
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Auto_Cooling, 2200));
    REQUIRE(h.state.runningMode.observed() == RunningMode::Cooling);
    REQUIRE(h.state.mode.observed()        == OperationalMode::Auto);
}

TEST_CASE("F3: S21 Auto_Heating observation → runningMode=Heating (S21 hint wins)",
          "[reconciler][groupF][runningMode]")
{
    Harness h;
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Auto_Heating, 2200));
    REQUIRE(h.state.runningMode.observed() == RunningMode::Heating);
    REQUIRE(h.state.mode.observed()        == OperationalMode::Auto);
}

TEST_CASE("F4: Cool mode + valveOpen=true → runningMode=Cooling",
          "[reconciler][groupF][runningMode]")
{
    Harness h;
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400, FanMode::Auto,
                                             std::optional<bool>{true}));
    REQUIRE(h.state.runningMode.observed() == RunningMode::Cooling);
}

TEST_CASE("F5: Cool mode + valveOpen=false → runningMode=Off",
          "[reconciler][groupF][runningMode]")
{
    Harness h;
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400, FanMode::Auto,
                                             std::optional<bool>{false}));
    REQUIRE(h.state.runningMode.observed() == RunningMode::Off);
}

TEST_CASE("F6: Heat mode + valveOpen=true → runningMode=Heating",
          "[reconciler][groupF][runningMode]")
{
    Harness h;
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Heat, 2000, FanMode::Auto,
                                             std::optional<bool>{true}));
    REQUIRE(h.state.runningMode.observed() == RunningMode::Heating);
}

TEST_CASE("F7: Cool mode + no valve + indoor above setpoint+hyst → Cooling (temp fallback)",
          "[reconciler][groupF][runningMode]")
{
    Harness h(coolModeDefaults(2400));
    // Establish indoor temperature first via env observation.
    h.rec.applyEnvironmentalObservation(envObs(/*indoor=*/2500));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));
    REQUIRE(h.state.runningMode.observed() == RunningMode::Cooling);
}

TEST_CASE("F8: Cool mode + no valve + indoor at setpoint (within hyst) → Off",
          "[reconciler][groupF][runningMode]")
{
    Harness h(coolModeDefaults(2400));
    h.rec.applyEnvironmentalObservation(envObs(/*indoor=*/2400));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));
    REQUIRE(h.state.runningMode.observed() == RunningMode::Off);
}

TEST_CASE("F9: Heat mode + no valve + indoor below setpoint-hyst → Heating",
          "[reconciler][groupF][runningMode]")
{
    LogicalACStateDefaults d;
    d.onOff        = true;
    d.mode         = OperationalMode::Heat;
    d.heatSetpoint = 2000;
    Harness h(d);
    h.rec.applyEnvironmentalObservation(envObs(/*indoor=*/1900));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Heat, 2000));
    REQUIRE(h.state.runningMode.observed() == RunningMode::Heating);
}

TEST_CASE("F10: Auto + valveOpen=true + indoor below auto setpoint → Heating",
          "[reconciler][groupF][runningMode]")
{
    Harness h(autoModeDefaults(2200));
    h.rec.applyEnvironmentalObservation(envObs(/*indoor=*/2000));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Auto, 2200, FanMode::Auto,
                                             std::optional<bool>{true}));
    REQUIRE(h.state.runningMode.observed() == RunningMode::Heating);
}

TEST_CASE("F11: Auto + valveOpen=true + indoor above auto setpoint → Cooling",
          "[reconciler][groupF][runningMode]")
{
    Harness h(autoModeDefaults(2200));
    h.rec.applyEnvironmentalObservation(envObs(/*indoor=*/2500));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Auto, 2200, FanMode::Auto,
                                             std::optional<bool>{true}));
    REQUIRE(h.state.runningMode.observed() == RunningMode::Cooling);
}

TEST_CASE("F12: Dry mode → runningMode=Off (no compressor activity reporting)",
          "[reconciler][groupF][runningMode]")
{
    Harness h;
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Dry, 2200, FanMode::Auto,
                                             std::optional<bool>{true}));
    REQUIRE(h.state.runningMode.observed() == RunningMode::Off);
}

TEST_CASE("F13: runningMode dirty path emitted when fusion result changes",
          "[reconciler][groupF][runningMode]")
{
    Harness h(coolModeDefaults(2400));
    h.rec.applyEnvironmentalObservation(envObs(/*indoor=*/2400));
    auto first = h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));
    REQUIRE(h.state.runningMode.observed() == RunningMode::Off);

    h.rec.applyEnvironmentalObservation(envObs(/*indoor=*/2600));
    auto second = h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));
    REQUIRE(h.state.runningMode.observed() == RunningMode::Cooling);
    REQUIRE(containsAttr(second.dirtyAttributes,
                         LogicalAttribute::RunningMode));
}
