/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#include <catch2/catch_test_macros.hpp>

#include "s21_to_matter_translator.h"

using SystemModeEnum  = chip::app::Clusters::Thermostat::SystemModeEnum;
using RunningModeEnum = chip::app::Clusters::Thermostat::ThermostatRunningModeEnum;

// ─── MockMatterSink ───────────────────────────────────────────────────────────

struct MockMatterSink : public MatterSink {
    // Recorded call arguments
    int onOffCallCount           = 0;
    int systemModeCallCount      = 0;
    int runningModeCallCount     = 0;
    int coolingSetpointCallCount = 0;
    int heatingSetpointCallCount = 0;
    int fanSpeedCallCount        = 0;
    int localTempCallCount       = 0;
    int outdoorTempCallCount     = 0;
    int humidityCallCount        = 0;

    bool              lastOnOff           = false;
    SystemModeEnum    lastSystemMode      = SystemModeEnum::kAuto;
    RunningModeEnum   lastRunningMode     = RunningModeEnum::kOff;
    int16_t           lastCoolingSetpoint = 0;
    int16_t           lastHeatingSetpoint = 0;
    std::optional<uint8_t> lastFanSpeed;
    int16_t           lastLocalTemp       = 0;
    int16_t           lastOutdoorTemp     = 0;
    uint16_t          lastHumidity        = 0;

    void setOnOff(bool v) override {
        onOffCallCount++;
        lastOnOff = v;
    }
    void setSystemMode(SystemModeEnum m) override {
        systemModeCallCount++;
        lastSystemMode = m;
    }
    void setRunningMode(RunningModeEnum m) override {
        runningModeCallCount++;
        lastRunningMode = m;
    }
    void setCoolingSetpoint(int16_t v) override {
        coolingSetpointCallCount++;
        lastCoolingSetpoint = v;
    }
    void setHeatingSetpoint(int16_t v) override {
        heatingSetpointCallCount++;
        lastHeatingSetpoint = v;
    }
    void setFanSpeedSetting(std::optional<uint8_t> s) override {
        fanSpeedCallCount++;
        lastFanSpeed = s;
    }
    void setLocalTemperature(int16_t v) override {
        localTempCallCount++;
        lastLocalTemp = v;
    }
    void setOutdoorTemperature(int16_t v) override {
        outdoorTempCallCount++;
        lastOutdoorTemp = v;
    }
    void setHumidity(uint16_t v) override {
        humidityCallCount++;
        lastHumidity = v;
    }
};

// ─── Helper ───────────────────────────────────────────────────────────────────

static S21State makeState(OperatingMode mode = OperatingMode::Cool,
                          int16_t setpoint = 2500,
                          int16_t indoor = 2600,
                          FanMode fan = FanMode::Auto)
{
    S21State s;
    s.onOff                         = true;
    s.operatingMode                 = mode;
    s.setpointCelsius               = setpoint;
    s.fanMode                       = fan;
    s.indoorTemperatureCelsius      = indoor;
    s.outdoorTemperatureCelsius     = 1000;
    s.indoorRelativeHumidityPercent = 60;
    return s;
}

// ─── SystemMode mapping ───────────────────────────────────────────────────────

TEST_CASE("OperatingMode::Cool maps to SystemModeEnum::kCool", "[translator][system_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Cool);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastSystemMode == SystemModeEnum::kCool);
}

TEST_CASE("OperatingMode::Heat maps to SystemModeEnum::kHeat", "[translator][system_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Heat, 2000, 2100);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastSystemMode == SystemModeEnum::kHeat);
}

TEST_CASE("OperatingMode::Dry maps to SystemModeEnum::kDry", "[translator][system_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Dry);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastSystemMode == SystemModeEnum::kDry);
}

TEST_CASE("OperatingMode::FanOnly maps to SystemModeEnum::kFanOnly", "[translator][system_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::FanOnly);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastSystemMode == SystemModeEnum::kFanOnly);
}

TEST_CASE("OperatingMode::Auto maps to SystemModeEnum::kAuto", "[translator][system_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Auto, 2200, 2200);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastSystemMode == SystemModeEnum::kAuto);
}

TEST_CASE("OperatingMode::Auto_Cooling maps to SystemModeEnum::kAuto", "[translator][system_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Auto_Cooling, 2200, 2300);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastSystemMode == SystemModeEnum::kAuto);
}

TEST_CASE("OperatingMode::Auto_Heating maps to SystemModeEnum::kAuto", "[translator][system_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Auto_Heating, 2200, 2100);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastSystemMode == SystemModeEnum::kAuto);
}

// ─── Setpoint writes by mode ──────────────────────────────────────────────────

TEST_CASE("Cool mode: only cooling setpoint written", "[translator][setpoints]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Cool, 2500, 2600);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.coolingSetpointCallCount == 1);
    REQUIRE(sink.lastCoolingSetpoint == 2500);
    REQUIRE(sink.heatingSetpointCallCount == 0);
}

TEST_CASE("Heat mode: only heating setpoint written", "[translator][setpoints]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Heat, 2000, 2100);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.heatingSetpointCallCount == 1);
    REQUIRE(sink.lastHeatingSetpoint == 2000);
    REQUIRE(sink.coolingSetpointCallCount == 0);
}

TEST_CASE("Auto mode: both setpoints written with deadband", "[translator][setpoints]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Auto, 2200, 2200);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.coolingSetpointCallCount == 1);
    REQUIRE(sink.lastCoolingSetpoint == 2250);
    REQUIRE(sink.heatingSetpointCallCount == 1);
    REQUIRE(sink.lastHeatingSetpoint == 2150);
}

TEST_CASE("Auto_Cooling mode: only cooling setpoint written with deadband", "[translator][setpoints]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Auto_Cooling, 2200, 2300);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.coolingSetpointCallCount == 1);
    REQUIRE(sink.lastCoolingSetpoint == 2250);
    REQUIRE(sink.heatingSetpointCallCount == 0);
}

TEST_CASE("Auto_Heating mode: only heating setpoint written with deadband", "[translator][setpoints]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Auto_Heating, 2200, 2100);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.heatingSetpointCallCount == 1);
    REQUIRE(sink.lastHeatingSetpoint == 2150);
    REQUIRE(sink.coolingSetpointCallCount == 0);
}

TEST_CASE("Dry mode: no setpoint written", "[translator][setpoints]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Dry);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.coolingSetpointCallCount == 0);
    REQUIRE(sink.heatingSetpointCallCount == 0);
}

TEST_CASE("FanOnly mode: no setpoint written", "[translator][setpoints]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::FanOnly);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.coolingSetpointCallCount == 0);
    REQUIRE(sink.heatingSetpointCallCount == 0);
}

// ─── RunningMode derivation ───────────────────────────────────────────────────

TEST_CASE("RunningMode: Cool, setpoint < indoor+50 => kCool", "[translator][running_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Cool, 2500, 2600);
    // setpoint(2500) < indoor(2600)+50 = 2650 → kCool
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastRunningMode == RunningModeEnum::kCool);
}

TEST_CASE("RunningMode: Cool, setpoint >= indoor+50 => kOff", "[translator][running_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Cool, 2700, 2600);
    // setpoint(2700) >= indoor(2600)+50 = 2650 → kOff
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastRunningMode == RunningModeEnum::kOff);
}

TEST_CASE("RunningMode: Cool boundary setpoint == indoor+50 => kOff", "[translator][running_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Cool, 2650, 2600);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastRunningMode == RunningModeEnum::kOff);
}

TEST_CASE("RunningMode: Cool boundary setpoint == indoor+49 => kCool", "[translator][running_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Cool, 2649, 2600);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastRunningMode == RunningModeEnum::kCool);
}

TEST_CASE("RunningMode: Heat, setpoint <= indoor-50 => kOff", "[translator][running_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Heat, 2000, 2100);
    // setpoint(2000) <= indoor(2100)-50 = 2050 → kOff
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastRunningMode == RunningModeEnum::kOff);
}

TEST_CASE("RunningMode: Heat, setpoint > indoor-50 => kHeat", "[translator][running_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Heat, 2100, 2100);
    // setpoint(2100) > indoor(2100)-50 = 2050 → kHeat
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastRunningMode == RunningModeEnum::kHeat);
}

TEST_CASE("RunningMode: Heat boundary setpoint == indoor-50 => kOff", "[translator][running_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Heat, 2050, 2100);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastRunningMode == RunningModeEnum::kOff);
}

TEST_CASE("RunningMode: Heat boundary setpoint == indoor-49 => kHeat", "[translator][running_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Heat, 2051, 2100);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastRunningMode == RunningModeEnum::kHeat);
}

TEST_CASE("RunningMode: Auto, indoor > setpoint+50 => kCool", "[translator][running_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Auto, 2200, 2300);
    // indoor(2300) > setpoint(2200)+50 = 2250 → kCool
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastRunningMode == RunningModeEnum::kCool);
}

TEST_CASE("RunningMode: Auto, indoor < setpoint-50 => kHeat", "[translator][running_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Auto, 2200, 2100);
    // indoor(2100) < setpoint(2200)-50 = 2150 → kHeat
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastRunningMode == RunningModeEnum::kHeat);
}

TEST_CASE("RunningMode: Auto, indoor within deadband => kOff", "[translator][running_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Auto, 2200, 2200);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastRunningMode == RunningModeEnum::kOff);
}

TEST_CASE("RunningMode: Auto, indoor == setpoint+50 => kOff (not >)", "[translator][running_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Auto, 2200, 2250);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastRunningMode == RunningModeEnum::kOff);
}

TEST_CASE("RunningMode: Auto, indoor == setpoint+51 => kCool (just above)", "[translator][running_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Auto, 2200, 2251);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastRunningMode == RunningModeEnum::kCool);
}

TEST_CASE("RunningMode: Auto, indoor == setpoint-50 => kOff (not <)", "[translator][running_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Auto, 2200, 2150);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastRunningMode == RunningModeEnum::kOff);
}

TEST_CASE("RunningMode: Auto, indoor == setpoint-51 => kHeat (just below)", "[translator][running_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Auto, 2200, 2149);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastRunningMode == RunningModeEnum::kHeat);
}

TEST_CASE("RunningMode: Auto_Cooling => always kCool", "[translator][running_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Auto_Cooling, 2200, 2300);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastRunningMode == RunningModeEnum::kCool);
}

TEST_CASE("RunningMode: Auto_Heating => always kHeat", "[translator][running_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Auto_Heating, 2200, 2100);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastRunningMode == RunningModeEnum::kHeat);
}

TEST_CASE("RunningMode: Dry => kOff", "[translator][running_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Dry);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastRunningMode == RunningModeEnum::kOff);
}

TEST_CASE("RunningMode: FanOnly => kOff", "[translator][running_mode]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::FanOnly);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastRunningMode == RunningModeEnum::kOff);
}

// ─── Fan speed mapping ────────────────────────────────────────────────────────

TEST_CASE("FanMode::Auto maps to nullopt (auto)", "[translator][fan]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Cool, 2500, 2600, FanMode::Auto);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE_FALSE(sink.lastFanSpeed.has_value());
}

TEST_CASE("FanMode::Quiet maps to speed 1", "[translator][fan]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Cool, 2500, 2600, FanMode::Quiet);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastFanSpeed == 1);
}

TEST_CASE("FanMode::Low maps to speed 2", "[translator][fan]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Cool, 2500, 2600, FanMode::Low);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastFanSpeed == 2);
}

TEST_CASE("FanMode::MidLow maps to speed 3", "[translator][fan]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Cool, 2500, 2600, FanMode::MidLow);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastFanSpeed == 3);
}

TEST_CASE("FanMode::Medium maps to speed 4", "[translator][fan]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Cool, 2500, 2600, FanMode::Medium);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastFanSpeed == 4);
}

TEST_CASE("FanMode::MidHigh maps to speed 5", "[translator][fan]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Cool, 2500, 2600, FanMode::MidHigh);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastFanSpeed == 5);
}

TEST_CASE("FanMode::High maps to speed 6", "[translator][fan]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Cool, 2500, 2600, FanMode::High);
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastFanSpeed == 6);
}

// ─── Sensor translation ───────────────────────────────────────────────────────

TEST_CASE("Sensor values passed through correctly", "[translator][sensors]")
{
    MockMatterSink sink;
    S21State s;
    s.onOff                         = false;
    s.operatingMode                 = OperatingMode::Cool;
    s.setpointCelsius               = 2500;
    s.fanMode                       = FanMode::Auto;
    s.indoorTemperatureCelsius      = 2500;
    s.outdoorTemperatureCelsius     = 1000;
    s.indoorRelativeHumidityPercent = 60;
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastLocalTemp   == 2500);
    REQUIRE(sink.lastOutdoorTemp == 1000);
    REQUIRE(sink.lastHumidity    == 6000);
}

TEST_CASE("Sensor: negative temperatures passed through unchanged", "[translator][sensors]")
{
    MockMatterSink sink;
    S21State s;
    s.operatingMode                 = OperatingMode::Cool;
    s.indoorTemperatureCelsius      = -500;
    s.outdoorTemperatureCelsius     = -1000;
    s.indoorRelativeHumidityPercent = 50;
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastLocalTemp   == -500);
    REQUIRE(sink.lastOutdoorTemp == -1000);
    REQUIRE(sink.lastHumidity    == 5000);
}

TEST_CASE("Sensor: zero humidity", "[translator][sensors]")
{
    MockMatterSink sink;
    S21State s;
    s.operatingMode                 = OperatingMode::Cool;
    s.indoorTemperatureCelsius      = 0;
    s.outdoorTemperatureCelsius     = 0;
    s.indoorRelativeHumidityPercent = 0;
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastHumidity == 0);
}

TEST_CASE("Sensor: 100% humidity multiplied to 10000", "[translator][sensors]")
{
    MockMatterSink sink;
    S21State s;
    s.operatingMode                 = OperatingMode::Cool;
    s.indoorTemperatureCelsius      = 2500;
    s.outdoorTemperatureCelsius     = 1000;
    s.indoorRelativeHumidityPercent = 100;
    S21ToMatterTranslator(s, sink).translate();
    REQUIRE(sink.lastHumidity == 10000);
}

// ─── Always-write ─────────────────────────────────────────────────────────────

TEST_CASE("Operation setters called on every translate(), even with identical state", "[translator][always_write]")
{
    MockMatterSink sink;
    S21State s = makeState(OperatingMode::Cool, 2500, 2600);
    S21ToMatterTranslator(s, sink).translate();
    S21ToMatterTranslator(s, sink).translate();

    REQUIRE(sink.onOffCallCount           == 2);
    REQUIRE(sink.systemModeCallCount      == 2);
    REQUIRE(sink.runningModeCallCount     == 2);
    REQUIRE(sink.coolingSetpointCallCount == 2);
    REQUIRE(sink.fanSpeedCallCount        == 2);
    REQUIRE(sink.localTempCallCount       == 2);
    REQUIRE(sink.outdoorTempCallCount     == 2);
    REQUIRE(sink.humidityCallCount        == 2);
}
