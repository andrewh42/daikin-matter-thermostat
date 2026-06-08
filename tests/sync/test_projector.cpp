/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Tests for sync::Projector. Cluster-side view of LogicalACState.
 */

#include <catch2/catch_test_macros.hpp>

#include "projector.h"

using namespace sync;

using SystemModeEnum            = chip::app::Clusters::Thermostat::SystemModeEnum;
using ThermostatRunningModeEnum = chip::app::Clusters::Thermostat::ThermostatRunningModeEnum;
using SetpointChangeSourceEnum  = chip::app::Clusters::Thermostat::SetpointChangeSourceEnum;
using FanModeEnum               = chip::app::Clusters::FanControl::FanModeEnum;

namespace {

LogicalACState stateAt(SystemModeEnum mode, int16_t heatSp, int16_t coolSp,
                       int16_t autoSp, int16_t indoor = 2300, int16_t outdoor = 1500,
                       uint8_t humidity = 50)
{
    LogicalACStateDefaults d;
    d.onOff        = true;
    d.mode         = mode;
    d.heatSetpoint = heatSp;
    d.coolSetpoint = coolSp;
    d.autoSetpoint = autoSp;
    d.indoorTemp   = indoor;
    d.outdoorTemp  = outdoor;
    d.humidity     = humidity;
    return LogicalACState(d);
}

} // namespace

// ─── Band projection ─────────────────────────────────────────────────────────

TEST_CASE("Cool mode: cluster cool = observed cool, cluster heat = shadow heat",
          "[phase3][projector]")
{
    Projector p;
    auto s = stateAt(SystemModeEnum::kCool, 2000, 2400, 2200);

    REQUIRE(p.projectedOccupiedCoolingSetpoint(s) == 2400);
    REQUIRE(p.projectedOccupiedHeatingSetpoint(s) == 2000);
}

TEST_CASE("Heat mode: cluster heat = observed heat, cluster cool = shadow cool",
          "[phase3][projector]")
{
    Projector p;
    auto s = stateAt(SystemModeEnum::kHeat, 2000, 2400, 2200);

    REQUIRE(p.projectedOccupiedHeatingSetpoint(s) == 2000);
    REQUIRE(p.projectedOccupiedCoolingSetpoint(s) == 2400);
}

TEST_CASE("Auto mode synthesises band from autoSetpoint ± δ",
          "[phase3][projector]")
{
    Projector p(ProjectorConfig{.autoBandHalfWidthCentiC = 50});
    auto s = stateAt(SystemModeEnum::kAuto, 2000, 2400, 2200);

    REQUIRE(p.projectedOccupiedHeatingSetpoint(s) == 2150);
    REQUIRE(p.projectedOccupiedCoolingSetpoint(s) == 2250);
}

TEST_CASE("Auto-band width is configurable", "[phase3][projector]")
{
    Projector wide(ProjectorConfig{.autoBandHalfWidthCentiC = 100});
    auto s = stateAt(SystemModeEnum::kAuto, 2000, 2400, 2200);

    REQUIRE(wide.projectedOccupiedHeatingSetpoint(s) == 2100);
    REQUIRE(wide.projectedOccupiedCoolingSetpoint(s) == 2300);
}

// ─── RunningMode derivation ──────────────────────────────────────────────────

TEST_CASE("RunningMode: Off when system mode is Off", "[phase3][projector]")
{
    Projector p;
    auto s = stateAt(SystemModeEnum::kOff, 2000, 2400, 2200);
    REQUIRE(p.projectedRunningMode(s) == ThermostatRunningModeEnum::kOff);
}

TEST_CASE("RunningMode in Cool: kCool when indoor is above setpoint+hyst",
          "[phase3][projector]")
{
    Projector p(ProjectorConfig{.runningModeHysteresisCentiC = 50});
    // Indoor 2500, setpoint 2400 → 2400+50 = 2450 < 2500 → kCool
    auto active = stateAt(SystemModeEnum::kCool, 2000, 2400, 2200, /*indoor=*/2500);
    REQUIRE(p.projectedRunningMode(active) == ThermostatRunningModeEnum::kCool);

    // Indoor 2400, setpoint 2400 → 2400+50 = 2450 >= 2400 → kOff
    auto idle = stateAt(SystemModeEnum::kCool, 2000, 2400, 2200, /*indoor=*/2400);
    REQUIRE(p.projectedRunningMode(idle) == ThermostatRunningModeEnum::kOff);
}

TEST_CASE("RunningMode in Heat: kHeat when indoor is below setpoint-hyst",
          "[phase3][projector]")
{
    Projector p(ProjectorConfig{.runningModeHysteresisCentiC = 50});
    // Indoor 1900, setpoint 2000 → 2000-50 = 1950 > 1900 → kHeat
    auto active = stateAt(SystemModeEnum::kHeat, 2000, 2400, 2200, /*indoor=*/1900);
    REQUIRE(p.projectedRunningMode(active) == ThermostatRunningModeEnum::kHeat);

    // Indoor 2000, setpoint 2000 → 2000-50 = 1950 <= 2000 → kOff
    auto idle = stateAt(SystemModeEnum::kHeat, 2000, 2400, 2200, /*indoor=*/2000);
    REQUIRE(p.projectedRunningMode(idle) == ThermostatRunningModeEnum::kOff);
}

TEST_CASE("RunningMode in Auto: derives kCool/kHeat/kOff from indoor vs auto target",
          "[phase3][projector]")
{
    Projector p(ProjectorConfig{.runningModeHysteresisCentiC = 50});

    auto cooling  = stateAt(SystemModeEnum::kAuto, 2000, 2400, 2200, /*indoor=*/2300);
    auto heating  = stateAt(SystemModeEnum::kAuto, 2000, 2400, 2200, /*indoor=*/2100);
    auto deadband = stateAt(SystemModeEnum::kAuto, 2000, 2400, 2200, /*indoor=*/2200);

    REQUIRE(p.projectedRunningMode(cooling)  == ThermostatRunningModeEnum::kCool);
    REQUIRE(p.projectedRunningMode(heating)  == ThermostatRunningModeEnum::kHeat);
    REQUIRE(p.projectedRunningMode(deadband) == ThermostatRunningModeEnum::kOff);
}

// ─── FanControl projection ───────────────────────────────────────────────────

TEST_CASE("FanControl: SpeedSetting nullopt ↔ FanMode kAuto with mid-range SpeedCurrent",
          "[phase3][projector]")
{
    Projector p;
    auto s = stateAt(SystemModeEnum::kCool, 2000, 2400, 2200);
    // Default fan is std::nullopt → Auto.
    REQUIRE_FALSE(p.projectedSpeedSetting(s).has_value());
    REQUIRE(p.projectedFanMode(s)     == FanModeEnum::kAuto);
    REQUIRE(p.projectedSpeedCurrent(s) == 3);
}

TEST_CASE("FanControl: SpeedSetting present → FanMode kOn, SpeedCurrent mirrors setting",
          "[phase3][projector]")
{
    Projector p;
    LogicalACStateDefaults d;
    d.fan = FanLevel::MidHigh; // SpeedSetting=5 on the wire
    LogicalACState s(d);

    REQUIRE(p.projectedSpeedSetting(s).has_value());
    REQUIRE(*p.projectedSpeedSetting(s) == FanLevel::MidHigh);
    REQUIRE(p.projectedFanMode(s)       == FanModeEnum::kOn);
    REQUIRE(p.projectedSpeedCurrent(s)  == 5);
}

// ─── LocalTemperature / OutdoorTemperature ───────────────────────────────────

TEST_CASE("LocalTemperature: nullopt before any observation; observed value passes through (incl. 0)",
          "[phase3][projector]")
{
    Projector p;

    // Default-constructed state has never been observed → nullopt.
    LogicalACState fresh{LogicalACStateDefaults{}};
    REQUIRE_FALSE(p.projectedLocalTemperature(fresh).has_value());

    // A genuine 0.00 °C reading is reported as 0, not null.
    auto z = stateAt(SystemModeEnum::kCool, 2000, 2400, 2200, /*indoor=*/0);
    REQUIRE(p.projectedLocalTemperature(z).has_value());
    REQUIRE(*p.projectedLocalTemperature(z) == 0);

    auto t = stateAt(SystemModeEnum::kCool, 2000, 2400, 2200, /*indoor=*/2350);
    REQUIRE(p.projectedLocalTemperature(t).has_value());
    REQUIRE(*p.projectedLocalTemperature(t) == 2350);
}

TEST_CASE("RunningMode: kOff when no indoor temperature observation",
          "[phase3][projector]")
{
    Projector p;
    LogicalACStateDefaults d;
    d.onOff = true;
    d.mode  = SystemModeEnum::kCool; // a mode that normally consults indoor
    LogicalACState s(d);
    REQUIRE(p.projectedRunningMode(s) == ThermostatRunningModeEnum::kOff);
}

TEST_CASE("RunningMode: kOff when device is powered off, regardless of mode",
          "[phase3][projector]")
{
    Projector p(ProjectorConfig{.runningModeHysteresisCentiC = 50});
    // Indoor 2500 vs cool setpoint 2400 would normally project kCool.
    auto s = stateAt(SystemModeEnum::kCool, 2000, 2400, 2200, /*indoor=*/2500);
    s.onOff.applyObservation(false, ObservationSource::Device);
    REQUIRE(p.projectedRunningMode(s) == ThermostatRunningModeEnum::kOff);
}

TEST_CASE("RunningMode: tracks desired onOff while a write is in flight",
          "[phase3][projector]")
{
    // Faithful-UI policy: with no in-flight, projection reflects desired;
    // once promoted to in-flight it reverts to observed until the device
    // acknowledges. Same as projectedOnOff. The RunningMode guard must
    // follow that policy so OnOff and RunningMode never disagree.
    Projector p(ProjectorConfig{.runningModeHysteresisCentiC = 50});
    auto s = stateAt(SystemModeEnum::kCool, 2000, 2400, 2200, /*indoor=*/2500);

    // Controller intent to power off: desired=false, observed still true.
    s.onOff.setDesired(false);
    REQUIRE(p.projectedRunningMode(s) == ThermostatRunningModeEnum::kOff);

    // Once the write is in flight, we report what the device confirmed.
    s.onOff.promoteDesiredToInFlight();
    REQUIRE(p.projectedRunningMode(s) == ThermostatRunningModeEnum::kCool);
}

TEST_CASE("HumidityCentiPercent: nullopt before any observation; ×100 of observed % otherwise",
          "[phase3][projector]")
{
    Projector p;

    LogicalACState fresh{LogicalACStateDefaults{}};
    REQUIRE_FALSE(p.projectedHumidityCentiPercent(fresh).has_value());

    LogicalACStateDefaults d;
    d.humidity = 50;
    LogicalACState s(d);
    REQUIRE(p.projectedHumidityCentiPercent(s).has_value());
    REQUIRE(*p.projectedHumidityCentiPercent(s) == 5000);
}

// ─── Diff ────────────────────────────────────────────────────────────────────

TEST_CASE("diffProjections emits only the cluster attributes that changed",
          "[phase3][projector][diff]")
{
    Projector p;
    auto a = stateAt(SystemModeEnum::kCool, 2000, 2400, 2200, /*indoor=*/2300);
    auto b = stateAt(SystemModeEnum::kCool, 2000, 2500, 2200, /*indoor=*/2300);

    auto paths = diffProjections(p.project(a), p.project(b), /*endpoint=*/1);
    REQUIRE(paths.size() == 1);
    REQUIRE(paths[0].endpoint  == 1);
    REQUIRE(paths[0].cluster   == chip::app::Clusters::Thermostat::Id);
    REQUIRE(paths[0].attribute ==
            chip::app::Clusters::Thermostat::Attributes::OccupiedCoolingSetpoint::Id);
}

// ─── refrigerantValveOpen signal ─────────────────────────────────────────────

namespace {

// Apply a valve observation to a copy of the state and return it.
LogicalACState withValve(LogicalACState s, std::optional<bool> valve)
{
    s.refrigerantValveOpen.applyObservation(valve);
    return s;
}

} // namespace

TEST_CASE("RunningMode: valve=false → kOff, overrides temperature heuristic in Cool mode",
          "[projector][valve]")
{
    Projector p(ProjectorConfig{.runningModeHysteresisCentiC = 50});
    // indoor=2600 vs cool setpoint=2400: heuristic would say kCool (2400+50=2450 < 2600)
    auto s = withValve(stateAt(SystemModeEnum::kCool, 2000, 2400, 2200, /*indoor=*/2600),
                       std::optional<bool>{false});
    REQUIRE(p.projectedRunningMode(s) == ThermostatRunningModeEnum::kOff);
}

TEST_CASE("RunningMode: valve=false → kOff, overrides temperature heuristic in Heat mode",
          "[projector][valve]")
{
    Projector p(ProjectorConfig{.runningModeHysteresisCentiC = 50});
    // indoor=1800 vs heat setpoint=2000: heuristic would say kHeat (2000-50=1950 > 1800)
    auto s = withValve(stateAt(SystemModeEnum::kHeat, 2000, 2400, 2200, /*indoor=*/1800),
                       std::optional<bool>{false});
    REQUIRE(p.projectedRunningMode(s) == ThermostatRunningModeEnum::kOff);
}

TEST_CASE("RunningMode: valve=true → kCool in Cool mode, regardless of temperature",
          "[projector][valve]")
{
    Projector p(ProjectorConfig{.runningModeHysteresisCentiC = 50});
    // indoor=2300 vs cool setpoint=2400: heuristic would say kOff (2400+50=2450 >= 2300)
    auto s = withValve(stateAt(SystemModeEnum::kCool, 2000, 2400, 2200, /*indoor=*/2300),
                       std::optional<bool>{true});
    REQUIRE(p.projectedRunningMode(s) == ThermostatRunningModeEnum::kCool);
}

TEST_CASE("RunningMode: valve=true → kHeat in Heat mode, regardless of temperature",
          "[projector][valve]")
{
    Projector p(ProjectorConfig{.runningModeHysteresisCentiC = 50});
    // indoor=2100 vs heat setpoint=2000: heuristic would say kOff (2000-50=1950 <= 2100)
    auto s = withValve(stateAt(SystemModeEnum::kHeat, 2000, 2400, 2200, /*indoor=*/2100),
                       std::optional<bool>{true});
    REQUIRE(p.projectedRunningMode(s) == ThermostatRunningModeEnum::kHeat);
}

TEST_CASE("RunningMode: valve=nullopt falls back to temperature heuristic (kCool)",
          "[projector][valve]")
{
    Projector p(ProjectorConfig{.runningModeHysteresisCentiC = 50});
    // indoor=2600 > 2400+50=2450 → kCool from heuristic
    auto s = withValve(stateAt(SystemModeEnum::kCool, 2000, 2400, 2200, /*indoor=*/2600),
                       std::nullopt);
    REQUIRE(p.projectedRunningMode(s) == ThermostatRunningModeEnum::kCool);
}

TEST_CASE("RunningMode: valve=nullopt falls back to temperature heuristic (kOff)",
          "[projector][valve]")
{
    Projector p(ProjectorConfig{.runningModeHysteresisCentiC = 50});
    // indoor=2300 ≤ 2400+50=2450 → kOff from heuristic
    auto s = withValve(stateAt(SystemModeEnum::kCool, 2000, 2400, 2200, /*indoor=*/2300),
                       std::nullopt);
    REQUIRE(p.projectedRunningMode(s) == ThermostatRunningModeEnum::kOff);
}

TEST_CASE("RunningMode: valve=true in Auto mode still uses temperature heuristic",
          "[projector][valve]")
{
    Projector p(ProjectorConfig{.runningModeHysteresisCentiC = 50});
    // Valve open but direction unknown in Auto; indoor=2600 > autoSetpoint=2200+50=2250 → kCool
    auto s = withValve(stateAt(SystemModeEnum::kAuto, 2000, 2400, 2200, /*indoor=*/2600),
                       std::optional<bool>{true});
    REQUIRE(p.projectedRunningMode(s) == ThermostatRunningModeEnum::kCool);
}

TEST_CASE("RunningMode: valve=false in Auto mode → kOff, overrides temperature heuristic",
          "[projector][valve]")
{
    Projector p(ProjectorConfig{.runningModeHysteresisCentiC = 50});
    // indoor=2600 vs auto setpoint=2200: heuristic would say kCool, but valve=false wins
    auto s = withValve(stateAt(SystemModeEnum::kAuto, 2000, 2400, 2200, /*indoor=*/2600),
                       std::optional<bool>{false});
    REQUIRE(p.projectedRunningMode(s) == ThermostatRunningModeEnum::kOff);
}

TEST_CASE("diffProjections returns empty when projections are equal",
          "[phase3][projector][diff]")
{
    Projector p;
    auto s = stateAt(SystemModeEnum::kCool, 2000, 2400, 2200);
    REQUIRE(diffProjections(p.project(s), p.project(s), /*endpoint=*/1).empty());
}

TEST_CASE("Auto-band mode flip yields heating/cooling/system-mode dirty attrs",
          "[phase3][projector][diff]")
{
    Projector p;
    auto cool = stateAt(SystemModeEnum::kCool, 2000, 2400, 2200);
    auto auto_ = stateAt(SystemModeEnum::kAuto, 2000, 2400, 2200);

    auto paths = diffProjections(p.project(cool), p.project(auto_), /*endpoint=*/1);
    // SystemMode, OccupiedHeating, OccupiedCooling all differ.
    auto contains = [&](chip::AttributeId a) {
        for (auto& p : paths) if (p.attribute == a) return true;
        return false;
    };
    REQUIRE(contains(chip::app::Clusters::Thermostat::Attributes::SystemMode::Id));
    REQUIRE(contains(chip::app::Clusters::Thermostat::Attributes::OccupiedHeatingSetpoint::Id));
    REQUIRE(contains(chip::app::Clusters::Thermostat::Attributes::OccupiedCoolingSetpoint::Id));
}
