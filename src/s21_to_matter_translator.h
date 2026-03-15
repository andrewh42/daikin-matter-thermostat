/*
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#pragma once

#include "s21/s21_presentation.h"

#include <app-common/zap-generated/attributes/Accessors.h>

#include <optional>
#include <stdint.h>

inline constexpr int16_t kDeadbandOffsetCelsius = 50; // 0.5 °C in 0.01 °C units

struct S21State {
    bool          onOff                         = false;
    OperatingMode operatingMode                 = OperatingMode::Auto;
    int16_t       setpointCelsius               = 2600; // 0.01 °C, S21 native single setpoint
    FanMode       fanMode                       = FanMode::Auto;
    int16_t       indoorTemperatureCelsius      = 0;    // 0.01 °C; also for RunningMode
    int16_t       outdoorTemperatureCelsius     = 0;    // 0.01 °C
    uint8_t       indoorRelativeHumidityPercent = 0;    // 0–100 %; translator ×100 for Matter
};

class MatterSink {
public:
    virtual ~MatterSink() = default;
    virtual void setOnOff(bool) = 0;
    virtual void setSystemMode(chip::app::Clusters::Thermostat::SystemModeEnum) = 0;
    virtual void setRunningMode(chip::app::Clusters::Thermostat::ThermostatRunningModeEnum) = 0;
    virtual void setCoolingSetpoint(int16_t hundredthsCelsius) = 0;
    virtual void setHeatingSetpoint(int16_t hundredthsCelsius) = 0;
    virtual void setFanSpeedSetting(std::optional<uint8_t> speedSetting) = 0; // nullopt → Auto
    virtual void setLocalTemperature(int16_t hundredthsCelsius) = 0;
    virtual void setOutdoorTemperature(int16_t hundredthsCelsius) = 0;
    virtual void setHumidity(uint16_t hundredthsPercent) = 0;
};

class S21ToMatterTranslator {
public:
    S21ToMatterTranslator(const S21State& source, MatterSink& sink);
    void translate(); // must be called while LockChipStack() is held

private:
    const S21State& mSource;
    MatterSink&     mSink;

    void translateOperation();
    void translateSensors();

    static chip::app::Clusters::Thermostat::SystemModeEnum
        OperatingModeToSystemMode(OperatingMode);
    chip::app::Clusters::Thermostat::ThermostatRunningModeEnum
        OperatingModeToRunningMode(OperatingMode, int16_t setpoint) const;
    static std::optional<uint8_t> S21FanModeToSpeedSetting(FanMode);
};
