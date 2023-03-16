/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#pragma once

#include "s21/s21_manager.h"
#include "matter_to_s21_translator.h"
#include "s21_to_matter_translator.h"

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
    void LogMatterThermostatStatus();

  private:
    // ClusterMatterSink: concrete MatterSink that writes to ZCL cluster attributes.
    // Defined and implemented in airconditioner_manager.cpp.
    class ClusterMatterSink : public MatterSink {
    public:
        explicit ClusterMatterSink(chip::EndpointId ep) : mEndpointId(ep) {}
        void setOnOff(bool v) override;
        void setSystemMode(chip::app::Clusters::Thermostat::SystemModeEnum m) override;
        void setRunningMode(chip::app::Clusters::Thermostat::ThermostatRunningModeEnum m) override;
        void setCoolingSetpoint(int16_t v) override;
        void setHeatingSetpoint(int16_t v) override;
        void setFanSpeedSetting(std::optional<uint8_t> s) override;
        void setLocalTemperature(int16_t v) override;
        void setOutdoorTemperature(int16_t v) override;
        void setHumidity(uint16_t v) override;
    private:
        chip::EndpointId mEndpointId;
    };

    static constexpr chip::EndpointId kThermostatEndpoint = 1;

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
    void Poll();
    void ExecutePendingCommands();
    static const char* GetSystemModeStr(app::Clusters::Thermostat::SystemModeEnum mode);
    static const char* GetRunningModeStr(app::Clusters::Thermostat::ThermostatRunningModeEnum mode);


    // Thread safety model:
    //
    // mPendingCommandFlags is std::atomic and accessed from both threads via fetch_or/exchange.
    // mUpdatingFromPoll is set on the Matter thread only (PostTask lambda); no concurrent access.
    std::atomic<uint32_t> mPendingCommandFlags{0};

    enum CommandFlags : uint32_t {
        kCommandOperation = BIT(0), // OnOff, mode, setpoint, fan — sent as a single setOperation()
    };

    int  mInitRetryIntervalMs{kS21InitRetryInitialIntervalMilliSec};

    // Last command sent to S21; used for no-change deduplication in ExecutePendingCommands().
    // Accessed only from the S21 work queue thread.
    std::optional<S21OperationCommand> mLastSentCommand;

    // Set true while a poll-driven translate() call is in progress to suppress spurious
    // command queuing from ZCL attribute-change callbacks. Accessed only on the Matter thread.
    bool mUpdatingFromPoll{false};

    ClusterMatterSink mMatterSink{kThermostatEndpoint};

    void OnOffAttributeChangeHandler(AttributeId attributeId, uint8_t* value, uint16_t size);
    void TemperatureAttributeChangeHandler(AttributeId attributeId, uint8_t* value, uint16_t size);
    void FanControlAttributeChangeHandler(AttributeId attributeId, uint8_t* value, uint16_t size);
    CHIP_ERROR InitLed();
    void UpdatePowerIndicator(bool onOff);
};
