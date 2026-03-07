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

    static constexpr int kS21PollIntervalSec                  = 30;
    static constexpr int kS21InitRetryInitialIntervalMilliSec = 500;
    static constexpr int kS21InitRetryMaximumIntervalMilliSec = 60'000;
    static void PollWorkHandler(k_work* work);
    static void InitRetryWorkHandler(k_work* work);
    void PollTemperatures();
    void PollOperation();
    static Clusters::Thermostat::SystemModeEnum OperatingModeToSystemMode(OperatingMode mode);

    int  mInitRetryIntervalMs{kS21InitRetryInitialIntervalMilliSec};
    bool mOnOff;
    DataModel::Nullable<int16_t> mLocalTempCelsius;
    DataModel::Nullable<int16_t> mOutdoorTempCelsius;
    int16_t mCoolingCelsiusSetPoint;
    int16_t mHeatingCelsiusSetPoint;
    Clusters::Thermostat::SystemModeEnum mThermMode;

    void OnOffAttributeChangeHandler(AttributeId attributeId, uint8_t* value, uint16_t size);
    void TemperatureAttributeChangeHandler(AttributeId attributeId, uint8_t* value, uint16_t size);
    CHIP_ERROR InitLed();
    const char* GetThermModeStr();
    void UpdatePowerIndicator();
};
