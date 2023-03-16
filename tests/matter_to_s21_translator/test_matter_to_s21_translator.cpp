/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#include <catch2/catch_test_macros.hpp>

#include "matter_to_s21_translator.h"

using SystemModeEnum = chip::app::Clusters::Thermostat::SystemModeEnum;

// ─── Helper ───────────────────────────────────────────────────────────────────

using NullableU8 = chip::app::DataModel::Nullable<uint8_t>;

static MatterState makeState(SystemModeEnum mode         = SystemModeEnum::kCool,
                              int16_t       cooling      = 2500,
                              int16_t       heating      = 2000,
                              NullableU8    speedSetting = NullableU8::Null())
{
    MatterState s;
    s.onOff                  = true;
    s.systemMode             = mode;
    s.coolingSetpointCelsius = cooling;
    s.heatingSetpointCelsius = heating;
    s.speedSetting           = speedSetting;
    return s;
}

// ─── OperatingMode mapping ────────────────────────────────────────────────────

TEST_CASE("kCool maps to OperatingMode::Cool", "[translator][mode]")
{
    auto cmd = MatterToS21Translator::translate(makeState(SystemModeEnum::kCool));
    REQUIRE(cmd.operatingMode == OperatingMode::Cool);
}

TEST_CASE("kHeat maps to OperatingMode::Heat", "[translator][mode]")
{
    auto cmd = MatterToS21Translator::translate(makeState(SystemModeEnum::kHeat));
    REQUIRE(cmd.operatingMode == OperatingMode::Heat);
}

TEST_CASE("kDry maps to OperatingMode::Dry", "[translator][mode]")
{
    auto cmd = MatterToS21Translator::translate(makeState(SystemModeEnum::kDry));
    REQUIRE(cmd.operatingMode == OperatingMode::Dry);
}

TEST_CASE("kFanOnly maps to OperatingMode::FanOnly", "[translator][mode]")
{
    auto cmd = MatterToS21Translator::translate(makeState(SystemModeEnum::kFanOnly));
    REQUIRE(cmd.operatingMode == OperatingMode::FanOnly);
}

TEST_CASE("kAuto maps to OperatingMode::Auto", "[translator][mode]")
{
    auto cmd = MatterToS21Translator::translate(makeState(SystemModeEnum::kAuto));
    REQUIRE(cmd.operatingMode == OperatingMode::Auto);
}

TEST_CASE("kEmergencyHeat maps to OperatingMode::Heat", "[translator][mode]")
{
    auto cmd = MatterToS21Translator::translate(makeState(SystemModeEnum::kEmergencyHeat));
    REQUIRE(cmd.operatingMode == OperatingMode::Heat);
}

TEST_CASE("kPrecooling maps to OperatingMode::Cool", "[translator][mode]")
{
    auto cmd = MatterToS21Translator::translate(makeState(SystemModeEnum::kPrecooling));
    REQUIRE(cmd.operatingMode == OperatingMode::Cool);
}

// ─── Setpoint derivation ──────────────────────────────────────────────────────

TEST_CASE("Cool: setpoint = coolingSetpoint", "[translator][setpoint]")
{
    auto cmd = MatterToS21Translator::translate(makeState(SystemModeEnum::kCool, 2500, 2000));
    REQUIRE(cmd.setpointCelsius == 2500);
}

TEST_CASE("Heat: setpoint = heatingSetpoint", "[translator][setpoint]")
{
    auto cmd = MatterToS21Translator::translate(makeState(SystemModeEnum::kHeat, 2500, 2000));
    REQUIRE(cmd.setpointCelsius == 2000);
}

TEST_CASE("Auto: setpoint = coolingSetpoint - deadband (50)", "[translator][setpoint]")
{
    // coolingSetpoint = S21setpoint + 50 (from S21ToMatterTranslator Auto path)
    // so S21setpoint = coolingSetpoint - 50
    auto cmd = MatterToS21Translator::translate(makeState(SystemModeEnum::kAuto, 2250, 2150));
    REQUIRE(cmd.setpointCelsius == 2200);
}

TEST_CASE("Dry: setpoint = coolingSetpoint", "[translator][setpoint]")
{
    auto cmd = MatterToS21Translator::translate(makeState(SystemModeEnum::kDry, 2500, 2000));
    REQUIRE(cmd.setpointCelsius == 2500);
}

TEST_CASE("FanOnly: setpoint = coolingSetpoint", "[translator][setpoint]")
{
    auto cmd = MatterToS21Translator::translate(makeState(SystemModeEnum::kFanOnly, 2500, 2000));
    REQUIRE(cmd.setpointCelsius == 2500);
}

// ─── Round-trip: S21→Matter→S21 setpoint is identity for Auto ─────────────────

TEST_CASE("Auto mode round-trip: S21 setpoint survives S21→Matter→S21", "[translator][roundtrip]")
{
    // S21ToMatterTranslator applied Auto split: cooling = 2200+50=2250, heating = 2200-50=2150
    // MatterToS21Translator should recover the original S21 setpoint: 2250-50=2200
    auto cmd = MatterToS21Translator::translate(makeState(SystemModeEnum::kAuto, 2250, 2150));
    REQUIRE(cmd.setpointCelsius == 2200);
}

// ─── SpeedSetting → FanMode translation ──────────────────────────────────────

TEST_CASE("null SpeedSetting → FanMode::Auto", "[translator][fan]")
{
    auto cmd = MatterToS21Translator::translate(makeState(SystemModeEnum::kCool, 2500, 2000, NullableU8::Null()));
    REQUIRE(cmd.fanMode == FanMode::Auto);
}

TEST_CASE("SpeedSetting 1 → FanMode::Quiet", "[translator][fan]")
{
    auto cmd = MatterToS21Translator::translate(makeState(SystemModeEnum::kCool, 2500, 2000, NullableU8::NonNull(1)));
    REQUIRE(cmd.fanMode == FanMode::Quiet);
}

TEST_CASE("SpeedSetting 2 → FanMode::Low", "[translator][fan]")
{
    auto cmd = MatterToS21Translator::translate(makeState(SystemModeEnum::kCool, 2500, 2000, NullableU8::NonNull(2)));
    REQUIRE(cmd.fanMode == FanMode::Low);
}

TEST_CASE("SpeedSetting 3 → FanMode::MidLow", "[translator][fan]")
{
    auto cmd = MatterToS21Translator::translate(makeState(SystemModeEnum::kCool, 2500, 2000, NullableU8::NonNull(3)));
    REQUIRE(cmd.fanMode == FanMode::MidLow);
}

TEST_CASE("SpeedSetting 4 → FanMode::Medium", "[translator][fan]")
{
    auto cmd = MatterToS21Translator::translate(makeState(SystemModeEnum::kCool, 2500, 2000, NullableU8::NonNull(4)));
    REQUIRE(cmd.fanMode == FanMode::Medium);
}

TEST_CASE("SpeedSetting 5 → FanMode::MidHigh", "[translator][fan]")
{
    auto cmd = MatterToS21Translator::translate(makeState(SystemModeEnum::kCool, 2500, 2000, NullableU8::NonNull(5)));
    REQUIRE(cmd.fanMode == FanMode::MidHigh);
}

TEST_CASE("SpeedSetting 6 → FanMode::High", "[translator][fan]")
{
    auto cmd = MatterToS21Translator::translate(makeState(SystemModeEnum::kCool, 2500, 2000, NullableU8::NonNull(6)));
    REQUIRE(cmd.fanMode == FanMode::High);
}

// ─── OnOff pass-through ───────────────────────────────────────────────────────

TEST_CASE("onOff=true passes through", "[translator][onoff]")
{
    MatterState s = makeState();
    s.onOff = true;
    REQUIRE(MatterToS21Translator::translate(s).onOff == true);
}

TEST_CASE("onOff=false passes through", "[translator][onoff]")
{
    MatterState s = makeState();
    s.onOff = false;
    REQUIRE(MatterToS21Translator::translate(s).onOff == false);
}

// ─── S21OperationCommand equality ────────────────────────────────────────────

TEST_CASE("S21OperationCommand: identical translations are equal", "[translator][equality]")
{
    auto cmd1 = MatterToS21Translator::translate(makeState(SystemModeEnum::kCool, 2500, 2000));
    auto cmd2 = MatterToS21Translator::translate(makeState(SystemModeEnum::kCool, 2500, 2000));
    REQUIRE(cmd1 == cmd2);
}

TEST_CASE("S21OperationCommand: different onOff are not equal", "[translator][equality]")
{
    MatterState a = makeState(); a.onOff = true;
    MatterState b = makeState(); b.onOff = false;
    REQUIRE_FALSE(MatterToS21Translator::translate(a) == MatterToS21Translator::translate(b));
}

TEST_CASE("S21OperationCommand: different mode are not equal", "[translator][equality]")
{
    auto cmd1 = MatterToS21Translator::translate(makeState(SystemModeEnum::kCool));
    auto cmd2 = MatterToS21Translator::translate(makeState(SystemModeEnum::kHeat));
    REQUIRE_FALSE(cmd1 == cmd2);
}

TEST_CASE("S21OperationCommand: different setpoints are not equal", "[translator][equality]")
{
    auto cmd1 = MatterToS21Translator::translate(makeState(SystemModeEnum::kCool, 2500));
    auto cmd2 = MatterToS21Translator::translate(makeState(SystemModeEnum::kCool, 2600));
    REQUIRE_FALSE(cmd1 == cmd2);
}

TEST_CASE("S21OperationCommand: different fan modes are not equal", "[translator][equality]")
{
    auto cmd1 = MatterToS21Translator::translate(makeState(SystemModeEnum::kCool, 2500, 2000, NullableU8::Null()));
    auto cmd2 = MatterToS21Translator::translate(makeState(SystemModeEnum::kCool, 2500, 2000, NullableU8::NonNull(6)));
    REQUIRE_FALSE(cmd1 == cmd2);
}

// ─── Unconditional output ─────────────────────────────────────────────────────

TEST_CASE("translate() is a pure function; same input always gives same output", "[translator][pure]")
{
    MatterState s = makeState(SystemModeEnum::kAuto, 2250, 2150, NullableU8::NonNull(4)); // 4 → Medium
    auto cmd1 = MatterToS21Translator::translate(s);
    auto cmd2 = MatterToS21Translator::translate(s);
    REQUIRE(cmd1 == cmd2);
    REQUIRE(cmd1.setpointCelsius == 2200);
    REQUIRE(cmd1.operatingMode  == OperatingMode::Auto);
    REQUIRE(cmd1.fanMode        == FanMode::Medium);
}
