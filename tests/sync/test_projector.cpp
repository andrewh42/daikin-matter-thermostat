/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Tests for sync::Projector. Cluster-side view of LogicalACState.
 *
 * RunningMode fusion lives in the reconciler (observation time); the
 * projector just maps state.runningMode → domain RunningMode with a !onOff
 * override. Tests here pin the mapping and the override; the fusion logic
 * is exercised by test_reconciler.cpp.
 */

#include <catch2/catch_test_macros.hpp>

#include "projector.h"
#include "logical_attribute.h"

#include <algorithm>

using namespace sync;

namespace {

LogicalACState stateAt(bool onOff, OperationalMode mode,
                       int16_t heatSp, int16_t coolSp, int16_t autoSp,
                       int16_t indoor = 2300, int16_t outdoor = 1500,
                       uint8_t humidity = 50,
                       RunningMode runningMode = RunningMode::Off)
{
    LogicalACStateDefaults d;
    d.onOff        = onOff;
    d.mode         = mode;
    d.heatSetpoint = heatSp;
    d.coolSetpoint = coolSp;
    d.autoSetpoint = autoSp;
    d.indoorTemp   = indoor;
    d.outdoorTemp  = outdoor;
    d.humidity     = humidity;
    d.runningMode  = runningMode;
    return LogicalACState(d);
}

bool contains(const std::vector<LogicalAttribute>& attrs, LogicalAttribute target)
{
    return std::any_of(attrs.begin(), attrs.end(),
        [&](LogicalAttribute a) { return a == target; });
}

} // namespace

// ─── Band projection ─────────────────────────────────────────────────────────

TEST_CASE("Cool mode: cluster cool = observed cool, cluster heat = shadow heat",
          "[phase3][projector]")
{
    Projector p;
    auto s = stateAt(true, OperationalMode::Cool, 2000, 2400, 2200);

    REQUIRE(p.projectedOccupiedCoolingSetpoint(s) == 2400);
    REQUIRE(p.projectedOccupiedHeatingSetpoint(s) == 2000);
}

TEST_CASE("Heat mode: cluster heat = observed heat, cluster cool = shadow cool",
          "[phase3][projector]")
{
    Projector p;
    auto s = stateAt(true, OperationalMode::Heat, 2000, 2400, 2200);

    REQUIRE(p.projectedOccupiedHeatingSetpoint(s) == 2000);
    REQUIRE(p.projectedOccupiedCoolingSetpoint(s) == 2400);
}

TEST_CASE("Auto mode synthesises band from autoSetpoint ± δ",
          "[phase3][projector]")
{
    Projector p(ProjectorConfig{.autoBandHalfWidthCentiC = 50});
    auto s = stateAt(true, OperationalMode::Auto, 2000, 2400, 2200);

    REQUIRE(p.projectedOccupiedHeatingSetpoint(s) == 2150);
    REQUIRE(p.projectedOccupiedCoolingSetpoint(s) == 2250);
}

TEST_CASE("Auto-band width is configurable", "[phase3][projector]")
{
    Projector wide(ProjectorConfig{.autoBandHalfWidthCentiC = 100});
    auto s = stateAt(true, OperationalMode::Auto, 2000, 2400, 2200);

    REQUIRE(wide.projectedOccupiedHeatingSetpoint(s) == 2100);
    REQUIRE(wide.projectedOccupiedCoolingSetpoint(s) == 2300);
}

// ─── Domain projection (no Matter translation) ───────────────────────────────

TEST_CASE("projectedOnOff and projectedMode return the kernel's twin values",
          "[phase3][projector]")
{
    Projector p;
    auto on  = stateAt(true,  OperationalMode::Cool, 2000, 2400, 2200);
    auto off = stateAt(false, OperationalMode::Heat, 2000, 2400, 2200);

    REQUIRE(p.projectedOnOff(on)   == true);
    REQUIRE(p.projectedMode(on)    == OperationalMode::Cool);
    REQUIRE(p.projectedOnOff(off)  == false);
    REQUIRE(p.projectedMode(off)   == OperationalMode::Heat);
}

// ─── RunningMode projection (just maps state.runningMode → domain enum) ──────

TEST_CASE("RunningMode: Off when power is off, regardless of state.runningMode",
          "[phase3][projector]")
{
    Projector p;
    auto s = stateAt(false, OperationalMode::Cool, 2000, 2400, 2200,
                     /*indoor=*/2300, /*outdoor=*/1500, /*humidity=*/50,
                     RunningMode::Cooling);
    REQUIRE(p.projectedRunningMode(s) == RunningMode::Off);
}

TEST_CASE("RunningMode: state.runningMode = Cooling → Cooling",
          "[phase3][projector]")
{
    Projector p;
    auto s = stateAt(true, OperationalMode::Cool, 2000, 2400, 2200,
                     /*indoor=*/2300, /*outdoor=*/1500, /*humidity=*/50,
                     RunningMode::Cooling);
    REQUIRE(p.projectedRunningMode(s) == RunningMode::Cooling);
}

TEST_CASE("RunningMode: state.runningMode = Heating → Heating",
          "[phase3][projector]")
{
    Projector p;
    auto s = stateAt(true, OperationalMode::Heat, 2000, 2400, 2200,
                     /*indoor=*/2100, /*outdoor=*/1500, /*humidity=*/50,
                     RunningMode::Heating);
    REQUIRE(p.projectedRunningMode(s) == RunningMode::Heating);
}

TEST_CASE("RunningMode: state.runningMode = Off → Off (powered on, idle)",
          "[phase3][projector]")
{
    Projector p;
    auto s = stateAt(true, OperationalMode::Cool, 2000, 2400, 2200,
                     /*indoor=*/2300, /*outdoor=*/1500, /*humidity=*/50,
                     RunningMode::Off);
    REQUIRE(p.projectedRunningMode(s) == RunningMode::Off);
}

TEST_CASE("RunningMode: tracks projected onOff while a power-off write is in flight",
          "[phase3][projector]")
{
    // Faithful-UI policy: while there's no in-flight, the projection
    // reflects desired. Once promoted to in-flight it reverts to observed
    // until the device acknowledges. The RunningMode guard follows the
    // same projectedOnOff policy so OnOff and RunningMode never disagree.
    Projector p;
    auto s = stateAt(true, OperationalMode::Cool, 2000, 2400, 2200,
                     /*indoor=*/2500, /*outdoor=*/1500, /*humidity=*/50,
                     RunningMode::Cooling);

    // Controller intent to power off: desired=false, observed still true.
    s.onOff.setDesired(false);
    REQUIRE(p.projectedRunningMode(s) == RunningMode::Off);

    // Once the write is in flight, we report what the device confirmed.
    s.onOff.promoteDesiredToInFlight();
    REQUIRE(p.projectedRunningMode(s) == RunningMode::Cooling);
}

// ─── FanControl projection ───────────────────────────────────────────────────

TEST_CASE("FanControl: SpeedSetting nullopt ↔ fanIsAuto with mid-range SpeedCurrent",
          "[phase3][projector]")
{
    Projector p;
    auto s = stateAt(true, OperationalMode::Cool, 2000, 2400, 2200);
    // Default fan is std::nullopt → Auto.
    REQUIRE_FALSE(p.projectedSpeedSetting(s).has_value());
    REQUIRE(p.projectedFanIsAuto(s) == true);
    REQUIRE(p.projectedSpeedCurrent(s) == 3);
}

TEST_CASE("FanControl: SpeedSetting present → fanIsAuto=false, SpeedCurrent mirrors setting",
          "[phase3][projector]")
{
    Projector p;
    LogicalACStateDefaults d;
    d.fan = FanLevel::MidHigh; // SpeedSetting=5 on the wire
    LogicalACState s(d);

    REQUIRE(p.projectedSpeedSetting(s).has_value());
    REQUIRE(*p.projectedSpeedSetting(s) == FanLevel::MidHigh);
    REQUIRE(p.projectedFanIsAuto(s)     == false);
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
    auto z = stateAt(true, OperationalMode::Cool, 2000, 2400, 2200, /*indoor=*/0);
    REQUIRE(p.projectedLocalTemperature(z).has_value());
    REQUIRE(*p.projectedLocalTemperature(z) == 0);

    auto t = stateAt(true, OperationalMode::Cool, 2000, 2400, 2200, /*indoor=*/2350);
    REQUIRE(p.projectedLocalTemperature(t).has_value());
    REQUIRE(*p.projectedLocalTemperature(t) == 2350);
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

TEST_CASE("diffProjections emits only the LogicalAttribute values that changed",
          "[phase3][projector][diff]")
{
    Projector p;
    auto a = stateAt(true, OperationalMode::Cool, 2000, 2400, 2200, /*indoor=*/2300);
    auto b = stateAt(true, OperationalMode::Cool, 2000, 2500, 2200, /*indoor=*/2300);

    auto attrs = diffProjections(p.project(a), p.project(b));
    REQUIRE(attrs.size() == 1);
    REQUIRE(attrs[0] == LogicalAttribute::OccupiedCoolingSetpoint);
}

TEST_CASE("diffProjections returns empty when projections are equal",
          "[phase3][projector][diff]")
{
    Projector p;
    auto s = stateAt(true, OperationalMode::Cool, 2000, 2400, 2200);
    REQUIRE(diffProjections(p.project(s), p.project(s)).empty());
}

TEST_CASE("OnOff change emits both OnOff and SystemMode (controller-visible SystemMode is derived)",
          "[phase3][projector][diff]")
{
    Projector p;
    auto on  = stateAt(true,  OperationalMode::Cool, 2000, 2400, 2200);
    auto off = stateAt(false, OperationalMode::Cool, 2000, 2400, 2200);

    auto attrs = diffProjections(p.project(on), p.project(off));
    REQUIRE(contains(attrs, LogicalAttribute::OnOff));
    REQUIRE(contains(attrs, LogicalAttribute::SystemMode));
}

TEST_CASE("Mode change (power on) emits SystemMode and not OnOff",
          "[phase3][projector][diff]")
{
    Projector p;
    auto cool = stateAt(true, OperationalMode::Cool, 2000, 2400, 2200);
    auto heat = stateAt(true, OperationalMode::Heat, 2000, 2400, 2200);

    auto attrs = diffProjections(p.project(cool), p.project(heat));
    REQUIRE_FALSE(contains(attrs, LogicalAttribute::OnOff));
    REQUIRE(contains(attrs, LogicalAttribute::SystemMode));
}

TEST_CASE("Auto-band mode flip yields heating/cooling/system-mode dirty attrs",
          "[phase3][projector][diff]")
{
    Projector p;
    auto cool  = stateAt(true, OperationalMode::Cool, 2000, 2400, 2200);
    auto auto_ = stateAt(true, OperationalMode::Auto, 2000, 2400, 2200);

    auto attrs = diffProjections(p.project(cool), p.project(auto_));
    REQUIRE(contains(attrs, LogicalAttribute::SystemMode));
    REQUIRE(contains(attrs, LogicalAttribute::OccupiedHeatingSetpoint));
    REQUIRE(contains(attrs, LogicalAttribute::OccupiedCoolingSetpoint));
}

TEST_CASE("Fan speed nullopt→value flip emits FanMode (Auto bit changes)",
          "[phase3][projector][diff]")
{
    Projector p;
    LogicalACStateDefaults a;
    a.onOff = true; a.mode = OperationalMode::Cool;
    LogicalACStateDefaults b = a;
    b.fan = FanLevel::Medium;

    LogicalACState before{a};
    LogicalACState after{b};

    auto attrs = diffProjections(p.project(before), p.project(after));
    REQUIRE(contains(attrs, LogicalAttribute::SpeedSetting));
    REQUIRE(contains(attrs, LogicalAttribute::FanMode));
    REQUIRE(contains(attrs, LogicalAttribute::SpeedCurrent));
}

TEST_CASE("Fan speed level→level change emits SpeedSetting and SpeedCurrent but NOT FanMode",
          "[phase3][projector][diff]")
{
    Projector p;
    LogicalACStateDefaults a;
    a.onOff = true; a.mode = OperationalMode::Cool; a.fan = FanLevel::Low;
    LogicalACStateDefaults b = a; b.fan = FanLevel::High;

    LogicalACState before{a};
    LogicalACState after{b};

    auto attrs = diffProjections(p.project(before), p.project(after));
    REQUIRE(contains(attrs, LogicalAttribute::SpeedSetting));
    REQUIRE(contains(attrs, LogicalAttribute::SpeedCurrent));
    REQUIRE_FALSE(contains(attrs, LogicalAttribute::FanMode));
}
