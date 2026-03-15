/*
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "s21_to_matter_translator.h"

using SystemModeEnum  = chip::app::Clusters::Thermostat::SystemModeEnum;
using RunningModeEnum = chip::app::Clusters::Thermostat::ThermostatRunningModeEnum;

S21ToMatterTranslator::S21ToMatterTranslator(const S21State& source, MatterSink& sink)
    : mSource(source), mSink(sink)
{
}

void S21ToMatterTranslator::translate()
{
    translateOperation();
    translateSensors();
}

void S21ToMatterTranslator::translateOperation()
{
    auto systemMode  = OperatingModeToSystemMode(mSource.operatingMode);
    auto runningMode = OperatingModeToRunningMode(mSource.operatingMode, mSource.setpointCelsius);

    mSink.setOnOff(mSource.onOff);
    mSink.setSystemMode(systemMode);
    mSink.setRunningMode(runningMode);

    switch (mSource.operatingMode) {
    case OperatingMode::Cool:
        mSink.setCoolingSetpoint(mSource.setpointCelsius);
        break;
    case OperatingMode::Heat:
        mSink.setHeatingSetpoint(mSource.setpointCelsius);
        break;
    case OperatingMode::Auto:
        mSink.setCoolingSetpoint(mSource.setpointCelsius + kDeadbandOffsetCelsius);
        mSink.setHeatingSetpoint(mSource.setpointCelsius - kDeadbandOffsetCelsius);
        break;
    case OperatingMode::Auto_Cooling:
        mSink.setCoolingSetpoint(mSource.setpointCelsius + kDeadbandOffsetCelsius);
        break;
    case OperatingMode::Auto_Heating:
        mSink.setHeatingSetpoint(mSource.setpointCelsius - kDeadbandOffsetCelsius);
        break;
    case OperatingMode::Dry:
    case OperatingMode::FanOnly:
        break;
    }

    mSink.setFanSpeedSetting(S21FanModeToSpeedSetting(mSource.fanMode));
}

void S21ToMatterTranslator::translateSensors()
{
    mSink.setLocalTemperature(mSource.indoorTemperatureCelsius);
    mSink.setOutdoorTemperature(mSource.outdoorTemperatureCelsius);
    mSink.setHumidity(static_cast<uint16_t>(mSource.indoorRelativeHumidityPercent) * 100);
}

SystemModeEnum S21ToMatterTranslator::OperatingModeToSystemMode(OperatingMode mode)
{
    switch (mode) {
    case OperatingMode::Cool:         return SystemModeEnum::kCool;
    case OperatingMode::Heat:         return SystemModeEnum::kHeat;
    case OperatingMode::Dry:          return SystemModeEnum::kDry;
    case OperatingMode::FanOnly:      return SystemModeEnum::kFanOnly;
    case OperatingMode::Auto:
    case OperatingMode::Auto_Cooling:
    case OperatingMode::Auto_Heating: return SystemModeEnum::kAuto;
    default:                          return SystemModeEnum::kAuto;
    }
}

RunningModeEnum S21ToMatterTranslator::OperatingModeToRunningMode(OperatingMode mode, int16_t setpoint) const
{
    int16_t localTemp = mSource.indoorTemperatureCelsius;

    switch (mode) {
    case OperatingMode::Auto_Cooling: return RunningModeEnum::kCool;
    case OperatingMode::Auto_Heating: return RunningModeEnum::kHeat;
    case OperatingMode::Cool:
        return (setpoint >= localTemp + 50) ? RunningModeEnum::kOff : RunningModeEnum::kCool;
    case OperatingMode::Heat:
        return (setpoint <= localTemp - 50) ? RunningModeEnum::kOff : RunningModeEnum::kHeat;
    case OperatingMode::Auto:
        if (localTemp > setpoint + 50) return RunningModeEnum::kCool;
        if (localTemp < setpoint - 50) return RunningModeEnum::kHeat;
        return RunningModeEnum::kOff;
    default:
        return RunningModeEnum::kOff;
    }
}

std::optional<uint8_t> S21ToMatterTranslator::S21FanModeToSpeedSetting(FanMode fanMode)
{
    switch (fanMode) {
    case FanMode::Auto:    return std::nullopt;
    case FanMode::Quiet:   return 1;
    case FanMode::Low:     return 2;
    case FanMode::MidLow:  return 3;
    case FanMode::Medium:  return 4;
    case FanMode::MidHigh: return 5;
    case FanMode::High:    return 6;
    default:               return std::nullopt;
    }
}
