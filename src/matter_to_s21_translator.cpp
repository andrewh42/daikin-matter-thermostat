/*
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "matter_to_s21_translator.h"

using SystemModeEnum = chip::app::Clusters::Thermostat::SystemModeEnum;
using chip::app::DataModel::Nullable;

S21OperationCommand MatterToS21Translator::translate(const MatterState& source)
{
    S21OperationCommand cmd;
    cmd.onOff   = source.onOff;
    cmd.fanMode = SpeedSettingToS21FanMode(source.speedSetting);

    cmd.operatingMode = SystemModeToOperatingMode(source.systemMode);

    switch (cmd.operatingMode) {
    case OperatingMode::Cool:
        cmd.setpointCelsius = source.coolingSetpointCelsius;
        break;
    case OperatingMode::Heat:
        cmd.setpointCelsius = source.heatingSetpointCelsius;
        break;
    case OperatingMode::Auto:
    case OperatingMode::Auto_Cooling:
    case OperatingMode::Auto_Heating:
        // Invert the deadband split applied in S21ToMatterTranslator: the S21
        // native setpoint sits kDeadbandOffsetCelsius below the cooling setpoint.
        cmd.setpointCelsius = source.coolingSetpointCelsius - kDeadbandOffsetCelsius;
        break;
    case OperatingMode::Dry:
    case OperatingMode::FanOnly:
        cmd.setpointCelsius = source.coolingSetpointCelsius;
        break;
    }

    return cmd;
}

FanMode MatterToS21Translator::SpeedSettingToS21FanMode(Nullable<uint8_t> speedSetting)
{
    if (speedSetting.IsNull()) return FanMode::Auto;
    switch (speedSetting.Value()) {
    case 0:  return FanMode::Auto; // 0 = power-off / unset → safe default
    case 1:  return FanMode::Quiet;
    case 2:  return FanMode::Low;
    case 3:  return FanMode::MidLow;
    case 4:  return FanMode::Medium;
    case 5:  return FanMode::MidHigh;
    case 6:  return FanMode::High;
    default: return FanMode::Auto; // out-of-range → Auto
    }
}

OperatingMode MatterToS21Translator::SystemModeToOperatingMode(SystemModeEnum mode)
{
    switch (mode) {
    case SystemModeEnum::kCool:          return OperatingMode::Cool;
    case SystemModeEnum::kHeat:          return OperatingMode::Heat;
    case SystemModeEnum::kEmergencyHeat: return OperatingMode::Heat;
    case SystemModeEnum::kDry:           return OperatingMode::Dry;
    case SystemModeEnum::kFanOnly:       return OperatingMode::FanOnly;
    case SystemModeEnum::kPrecooling:    return OperatingMode::Cool;
    case SystemModeEnum::kAuto:
    default:                             return OperatingMode::Auto;
    }
}
