/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Tests for sync::LogicalACState. The struct itself is dumb glue around
 * TwinField; these tests pin the wiring (defaults, mode→setpoint mapping,
 * per-mode write isolation).
 */

#include <catch2/catch_test_macros.hpp>

#include "logical_ac_state.h"

using sync::LogicalACState;
using sync::LogicalACStateDefaults;
using sync::OperationalMode;
using sync::RunningMode;

TEST_CASE("Defaulted state has power off, auto mode, and bracketed setpoints",
          "[phase1][logical_ac_state]")
{
    LogicalACState s;

    REQUIRE(s.onOff.observed()        == false);
    REQUIRE(s.mode.observed()         == OperationalMode::Auto);
    REQUIRE(s.heatSetpoint.observed() == 2000);
    REQUIRE(s.coolSetpoint.observed() == 2500);
    REQUIRE(s.autoSetpoint.observed() == 2200);
    REQUIRE_FALSE(s.fan.observed().has_value());
    REQUIRE(s.runningMode.observed()  == RunningMode::Off);
    REQUIRE(s.reachable.observed()    == false);
}

TEST_CASE("Defaults are propagated from LogicalACStateDefaults",
          "[phase1][logical_ac_state]")
{
    LogicalACStateDefaults d;
    d.onOff        = true;
    d.mode         = OperationalMode::Cool;
    d.heatSetpoint = 1900;
    d.coolSetpoint = 2400;
    d.autoSetpoint = 2100;
    d.fan          = sync::FanLevel::MidLow;
    d.runningMode  = RunningMode::Cooling;
    d.reachable    = true;

    LogicalACState s(d);
    REQUIRE(s.onOff.observed()        == true);
    REQUIRE(s.mode.observed()         == OperationalMode::Cool);
    REQUIRE(s.heatSetpoint.observed() == 1900);
    REQUIRE(s.coolSetpoint.observed() == 2400);
    REQUIRE(s.autoSetpoint.observed() == 2100);
    REQUIRE(s.fan.observed().has_value());
    REQUIRE(*s.fan.observed()         == sync::FanLevel::MidLow);
    REQUIRE(s.runningMode.observed()  == RunningMode::Cooling);
    REQUIRE(s.reachable.observed()    == true);
}

TEST_CASE("activeSetpoint routes by OperationalMode", "[phase1][logical_ac_state]")
{
    LogicalACState s;

    // Distinct desireds so we can tell them apart by reference identity.
    s.heatSetpoint.setDesired(1801);
    s.coolSetpoint.setDesired(2602);
    s.autoSetpoint.setDesired(2203);

    REQUIRE(s.activeSetpoint(OperationalMode::Heat).desired() == 1801);
    REQUIRE(s.activeSetpoint(OperationalMode::Cool).desired() == 2602);
    REQUIRE(s.activeSetpoint(OperationalMode::Auto).desired() == 2203);
}

TEST_CASE("activeSetpoint falls back to auto for modes with no setpoint concept",
          "[phase1][logical_ac_state]")
{
    LogicalACState s;
    s.autoSetpoint.setDesired(2100);
    REQUIRE(s.activeSetpoint(OperationalMode::Dry).desired()     == 2100);
    REQUIRE(s.activeSetpoint(OperationalMode::FanOnly).desired() == 2100);
}

TEST_CASE("Per-mode setpoint writes do not bleed across modes",
          "[phase1][logical_ac_state]")
{
    LogicalACState s;
    const auto heatBefore = s.heatSetpoint.observed();
    const auto autoBefore = s.autoSetpoint.observed();

    s.coolSetpoint.setDesired(2700);

    REQUIRE(s.coolSetpoint.desired()  == 2700);
    REQUIRE(s.heatSetpoint.observed() == heatBefore);
    REQUIRE(s.heatSetpoint.desired()  == heatBefore);
    REQUIRE(s.autoSetpoint.observed() == autoBefore);
    REQUIRE(s.autoSetpoint.desired()  == autoBefore);
}
