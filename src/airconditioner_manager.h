/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#pragma once

#include "s21/s21_manager.h"
#include "sync/changed_attributes_listener.h"
#include "sync/logical_attribute.h"
#include "sync/operational_mode.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <lib/core/CHIPError.h>

#include <optional>
#include <stdbool.h>
#include <stdint.h>
#include <vector>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

namespace sync { class SyncCoordinator; }

/**
 * AirConditionerManager owns the runtime mechanics that connect the S21
 * data link to the Matter stack:
 *
 *   - the S21 work queue and its three work items (init retry, poll, pump),
 *   - the command-pump handler that wakes the pump on SyncCoordinator mutations,
 *   - two ChangedAttributesListeners installed into SyncCoordinator:
 *       MatterReportListener   — drains dirty paths into the Matter event
 *                                loop and fires reporting callbacks under
 *                                the CHIP stack lock,
 *       PowerIndicatorListener — owns LED0 end-to-end (GPIO configure,
 *                                set, and the OnOff dirty-path handler).
 *
 * Bridge state (twins, intents, projections) lives in sync::SyncCoordinator.
 * Every controller-side write and every poll observation flows through
 * SyncCoordinator; this class is the choreographer, not the source of truth.
 */
class AirConditionerManager {
  public:
    static AirConditionerManager& Instance()
    {
        static AirConditionerManager sAirConditionerManager;
        return sAirConditionerManager;
    }

    /// One-time setup. Brings up the S21 work queue, starts the poll/init
    /// loop, wires the SyncCoordinator command-pump handler, and registers the
    /// two ChangedAttributesListeners (Matter reporter, power indicator).
    CHIP_ERROR Init(S21Manager& s21Manager, sync::SyncCoordinator& syncStack);

    /// Debug helpers retained for the existing Button 2 handler.
    void LogThermostatStatus();
    void LogMatterThermostatStatus();

    /// Cached sensor reads used by sensor.cpp's "external thermostat"
    /// readback path. Both go through SyncCoordinator now. `std::nullopt` mirrors
    /// the cluster-server's "null" state.
    std::optional<int16_t> GetLocalTemp();
    std::optional<int16_t> GetOutdoorTemp();

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
    /// state) fire every poll. At kS21PollIntervalSec = 10 s and N = 6,
    /// sensors refresh every ~1 minute — enough for steady-state HVAC use
    /// and a meaningful reduction in S21 traffic.
    static constexpr int kS21EnvironmentalSensorReadTicks = 6;

    /// Number of consecutive operational-poll failures before we flip the
    /// bridge's Reachable view to false. Three at the 10 s poll interval
    /// gives a ~30 s window — long enough to absorb a single transient bus
    /// glitch, short enough to surface a real outage before the controller's
    /// subscription timeout.
    static constexpr int kReachableFailureThreshold = 3;

    // ─── Listeners ───────────────────────────────────────────────────────

    /// Drains dirty paths into a member queue and posts the drain onto the
    /// Matter event loop, where MatterReportingAttributeChangeCallback is
    /// invoked under the CHIP stack lock. The queue absorbs bursts from
    /// the S21 work queue without blocking it.
    class MatterAttributeChangeReporter : public sync::ChangedAttributesListener {
    public:
        explicit MatterAttributeChangeReporter(AirConditionerManager& owner) : mOwner(owner)
        {
            k_mutex_init(&mQueueMutex);
        }
        void OnChangedAttributes(const std::vector<sync::LogicalAttribute>& attributes) override;
        void DrainOnMatterEventLoop();
    private:
        AirConditionerManager&                 mOwner;
        k_mutex                                mQueueMutex;
        std::vector<sync::LogicalAttribute>    mQueue;
    };

    /// Owns LED0. Configures the GPIO at boot, drives it from the OnOff
    /// dirty path on every transition. Both gpio_pin_set_dt and the
    /// SyncCoordinator read call are thread-safe, so no CHIP-event-loop hop is
    /// needed in the dispatch body.
    class PowerIndicator : public sync::ChangedAttributesListener {
    public:
        explicit PowerIndicator(AirConditionerManager& owner) : mOwner(owner) {}
        /// Configure LED0 and drive it to `initialOnOff`. Call once,
        /// before AddChangedAttributesListener — a dispatch arriving
        /// before Init returns would fire gpio_pin_set_dt on an
        /// unconfigured pin. Returns 0 on success or a negative Zephyr
        /// errno (-ENODEV if the GPIO isn't ready, or the value from
        /// gpio_pin_configure_dt on configure failure).
        int Init(bool initialOnOff);
        void OnChangedAttributes(const std::vector<sync::LogicalAttribute>& attributes) override;
    private:
        void Set(bool onOff);
        AirConditionerManager& mOwner;
        static const struct gpio_dt_spec sLed0;
    };

    int mConsecutivePollFailures{0};

    /// Ticks remaining until the next environmental phase. Initialised to 0
    /// so the first poll after boot performs both phases — controllers see
    /// a complete view at boot.
    int mEnvironmentalReadCountdown{0};

    S21Manager*             mS21Manager{nullptr};
    sync::SyncCoordinator*        mSyncCoordinator{nullptr};
    struct k_work_q         mS21WorkQueue;
    struct k_work_delayable mPollWork;
    struct k_work_delayable mInitRetryWork;
    struct k_work_delayable mCommandWork;
    int                     mInitRetryIntervalMs{kS21InitRetryInitialIntervalMilliSec};

    MatterAttributeChangeReporter    mMatterAttributeChangeReporter{*this};
    PowerIndicator  mPowerIndicator{*this};

    static void PollWorkHandler(k_work* work);
    static void InitRetryWorkHandler(k_work* work);
    static void CommandWorkHandler(k_work* work);

    void Poll();
    void PollEnvironmentalPhase();
    void PollOperationalPhase();

    /// SyncCoordinator command-pump handler: queue mCommandWork on the S21 work
    /// queue with a short debounce so consecutive intents collapse into
    /// one D1.
    static void ScheduleS21CommandHandler();

    static const char* GetOperationalModeStr(sync::OperationalMode);
    static const char* GetRunningModeStr(sync::RunningMode);
};
