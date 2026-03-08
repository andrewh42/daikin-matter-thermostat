/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#pragma once

#include "s21/s21_manager.h"

#include <stdbool.h>
#include <stdint.h>

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app/ConcreteAttributePath.h>

#include <lib/core/CHIPError.h>

#include <atomic>
#include <optional>
#include <zephyr/kernel.h>

using namespace chip;
using namespace chip::app;

class AirConditionerManager {
  public:
    static AirConditionerManager& Instance()
    {
        static AirConditionerManager sAirConditionerManager;
        return sAirConditionerManager;
    };

    CHIP_ERROR Init(S21Manager& s21Manager);
    void AttributeChangeHandler(const ConcreteAttributePath& attributePath, uint8_t* value, uint16_t size);
    DataModel::Nullable<int16_t> GetLocalTemp();
    DataModel::Nullable<int16_t> GetOutdoorTemp();

    void LogThermostatStatus();

  private:
    S21Manager* mS21Manager{nullptr};
    struct k_work_q         mS21WorkQueue;
    struct k_work_delayable mPollWork;
    struct k_work_delayable mInitRetryWork;
    struct k_work           mCommandWork;

    static constexpr int kS21PollIntervalSec                  = 15; // poll frequently to stress-test the UART code
    static constexpr int kS21InitRetryInitialIntervalMilliSec = 500;
    static constexpr int kS21InitRetryMaximumIntervalMilliSec = 60'000;
    static void PollWorkHandler(k_work* work);
    static void InitRetryWorkHandler(k_work* work);
    static void CommandWorkHandler(k_work* work);
    void PollSensors();
    void PollOperation();
    void ExecutePendingCommands();
    static Clusters::Thermostat::SystemModeEnum OperatingModeToSystemMode(OperatingMode mode);
    static Clusters::Thermostat::ThermostatRunningModeEnum OperatingModeToRunningMode(OperatingMode mode);
    static OperatingMode SystemModeToOperatingMode(Clusters::Thermostat::SystemModeEnum mode);
    static std::optional<FanMode> SpeedSettingToS21FanMode(uint8_t rawValue);
    static std::optional<uint8_t> S21FanModeToSpeedSetting(FanMode fanMode);

    // mPendingCommandFlags is written from the Matter thread (fetch_or) and from the S21 work
    // queue thread (exchange). All other mutable state is read/written only from the S21 work
    // queue thread (PollOperation, ExecutePendingCommands) and from the Matter callback thread
    // (AttributeChangeHandler). On ARM Cortex-M, aligned reads/writes of ≤4 bytes are
    // hardware-atomic; the compiler cannot cache struct member reads across opaque calls like
    // k_work_submit_to_queue, so these accesses are safe in practice without additional atomics.
    std::atomic<uint32_t> mPendingCommandFlags{0};

    enum CommandFlags : uint32_t {
        kCommandOperation = BIT(0), // OnOff, mode, setpoint, fan — sent as a single setOperation()
    };

    int  mInitRetryIntervalMs{kS21InitRetryInitialIntervalMilliSec};
    bool mOnOff{false};
    DataModel::Nullable<int16_t>  mLocalTempCelsius;
    DataModel::Nullable<int16_t>  mOutdoorTempCelsius;
    DataModel::Nullable<uint16_t> mHumidity;
    int16_t mCoolingCelsiusSetPoint{0};
    int16_t mHeatingCelsiusSetPoint{0};
    Clusters::Thermostat::SystemModeEnum mThermMode{Clusters::Thermostat::SystemModeEnum::kAuto};
    Clusters::Thermostat::ThermostatRunningModeEnum mRunningMode{Clusters::Thermostat::ThermostatRunningModeEnum::kOff};
    FanMode mFanMode{FanMode::Auto};

    void OnOffAttributeChangeHandler(AttributeId attributeId, uint8_t* value, uint16_t size);
    void TemperatureAttributeChangeHandler(AttributeId attributeId, uint8_t* value, uint16_t size);
    void FanControlAttributeChangeHandler(AttributeId attributeId, uint8_t* value, uint16_t size);
    CHIP_ERROR InitLed();
    const char* GetThermModeStr();
    void UpdatePowerIndicator();
};
