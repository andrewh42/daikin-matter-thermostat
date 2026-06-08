/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#pragma once

#include "s21/s21_manager.h"
#include "sync/matter_attribute_path.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <lib/core/CHIPError.h>

#include <stdbool.h>
#include <stdint.h>
#include <vector>
#include <zephyr/kernel.h>

namespace sync { class SyncStack; }

/**
 * AirConditionerManager owns the runtime mechanics that connect the S21
 * data link to the Matter stack:
 *
 *   - the S21 work queue and its three work items (init retry, poll, pump),
 *   - the LED0 power indicator,
 *   - the chip-stack-side dirty-attribute report flush.
 *
 * Bridge state (twins, intents, projections) lives in sync::SyncStack.
 * Every controller-side write and every poll observation flows through
 * SyncStack; this class is now the choreographer, not the source of
 * truth. See sync-option-4-plan.md Phase 7 for the cutover rationale.
 */
class AirConditionerManager {
  public:
    static AirConditionerManager& Instance()
    {
        static AirConditionerManager sAirConditionerManager;
        return sAirConditionerManager;
    }

    /// One-time setup. Brings up the S21 work queue, starts the poll/init
    /// loop, registers the LED indicator, and wires the SyncStack hooks
    /// that schedule the command pump and flush dirty-attribute reports.
    CHIP_ERROR Init(S21Manager& s21Manager, sync::SyncStack& syncStack);

    /// Debug helpers retained for the existing Button 2 handler.
    void LogThermostatStatus();
    void LogMatterThermostatStatus();

    /// Cached sensor reads used by sensor.cpp's "external thermostat"
    /// readback path. Both go through SyncStack now.
    chip::app::DataModel::Nullable<int16_t> GetLocalTemp();
    chip::app::DataModel::Nullable<int16_t> GetOutdoorTemp();

  private:
    AirConditionerManager() = default;

    static constexpr chip::EndpointId kThermostatEndpoint = 1;
    static constexpr int kS21PollIntervalSec                  = 10;
    static constexpr int kS21InitRetryInitialIntervalMilliSec = 500;
    static constexpr int kS21InitRetryMaximumIntervalMilliSec = 60'000;
    static constexpr int kCommandDebounceMs                   = 50;

    /// Cadence ratio of operational to environmental polls. Environmental
    /// reads (room/outdoor temperature, humidity) fire on every Nth poll
    /// where N = this constant; operational reads (op + conditional unit
    /// state) fire every poll. At kS21PollIntervalSec = 10 s and N = 12,
    /// sensors refresh every ~2 minutes — enough for steady-state HVAC use
    /// and a meaningful reduction in S21 traffic.
    static constexpr int kS21EnvironmentalSensorReadTicks = 12;

    /// Number of consecutive operational-poll failures before we flip the
    /// bridge's Reachable view to false. Three at the 10 s poll interval
    /// gives a ~30 s window — long enough to absorb a single transient bus
    /// glitch, short enough to surface a real outage before the controller's
    /// subscription timeout.
    static constexpr int kReachableFailureThreshold = 3;

    int mConsecutivePollFailures{0};

    /// Ticks remaining until the next environmental phase. Initialised to 0
    /// so the first poll after boot performs both phases — controllers see
    /// a complete view at boot.
    int mEnvironmentalReadCountdown{0};

    S21Manager*             mS21Manager{nullptr};
    sync::SyncStack*        mSyncStack{nullptr};
    struct k_work_q         mS21WorkQueue;
    struct k_work_delayable mPollWork;
    struct k_work_delayable mInitRetryWork;
    struct k_work_delayable mCommandWork;
    int                     mInitRetryIntervalMs{kS21InitRetryInitialIntervalMilliSec};

    static void PollWorkHandler(k_work* work);
    static void InitRetryWorkHandler(k_work* work);
    static void CommandWorkHandler(k_work* work);

    void Poll();
    void PollEnvironmentalPhase();
    void PollOperationalPhase();

    /// SyncStack hook: queue mCommandWork on the S21 work queue with a
    /// short debounce so consecutive intents collapse into one D1.
    static void ScheduleS21CommandHook();

    /// SyncStack hook: forward dirty paths to the Matter event loop and
    /// fire MatterReportingAttributeChangeCallback for each.
    static void ReportDirtyAttributesHook(const std::vector<sync::MatterAttributePath>& paths);

    CHIP_ERROR InitLed();
    void       UpdatePowerIndicator(bool onOff);

    static const char* GetSystemModeStr(chip::app::Clusters::Thermostat::SystemModeEnum);
    static const char* GetRunningModeStr(chip::app::Clusters::Thermostat::ThermostatRunningModeEnum);
};
