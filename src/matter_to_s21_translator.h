/*
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#pragma once

#include "s21/s21_presentation.h"
#include "s21_to_matter_translator.h" // kDeadbandOffsetCelsius

#include <app-common/zap-generated/attributes/Accessors.h>

#include <stdint.h>

// The Matter-side state that drives an outbound S21 setOperation() call.
struct MatterState {
    bool          onOff                  = false;
    chip::app::Clusters::Thermostat::SystemModeEnum
                  systemMode             = chip::app::Clusters::Thermostat::SystemModeEnum::kAuto;
    int16_t       coolingSetpointCelsius = 2600; // 0.01 °C
    int16_t       heatingSetpointCelsius = 2000; // 0.01 °C
    chip::app::DataModel::Nullable<uint8_t> speedSetting; // null → Auto
};

// The S21 command produced by translation.  Supports equality comparison so
// callers can detect no-change without resending.
struct S21OperationCommand {
    bool          onOff           = false;
    OperatingMode operatingMode   = OperatingMode::Auto;
    int16_t       setpointCelsius = 2600; // 0.01 °C, single S21 setpoint
    FanMode       fanMode         = FanMode::Auto;

    bool operator==(const S21OperationCommand& o) const
    {
        return onOff == o.onOff &&
               operatingMode == o.operatingMode &&
               setpointCelsius == o.setpointCelsius &&
               fanMode == o.fanMode;
    }
};

class MatterToS21Translator {
public:
    // Pure translation: convert a MatterState to an S21OperationCommand.
    // No state is retained; call this any time you need the translated command.
    // No-change detection belongs to the caller (compare the returned struct).
    static S21OperationCommand translate(const MatterState& source);

private:
    static OperatingMode SystemModeToOperatingMode(
        chip::app::Clusters::Thermostat::SystemModeEnum mode);
    static FanMode SpeedSettingToS21FanMode(
        chip::app::DataModel::Nullable<uint8_t> speedSetting);
};
