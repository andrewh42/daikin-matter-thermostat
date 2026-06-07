/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "airconditioner_manager.h"
#include "sync/sync_stack.h"

#include "app/task_executor.h"

#include <app-common/zap-generated/cluster-objects.h>
#include <app/reporting/reporting.h>
#include <platform/PlatformManager.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <optional>
#include <utility>

LOG_MODULE_REGISTER(aircon_mgr, LOG_LEVEL_DBG);

using namespace chip;
using namespace ::chip::DeviceLayer;
using namespace ::chip::app::Clusters;
using namespace ::chip::app::Clusters::Thermostat::Attributes;
using namespace Protocols::InteractionModel;

K_THREAD_STACK_DEFINE(sS21WorkQueueStack, 2048);

namespace {

const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

uint8_t ReturnCompleteValue(int16_t v)  { return static_cast<uint8_t>(v / 100); }
uint8_t ReturnRemainderValue(int16_t v) { return static_cast<uint8_t>((v % 100 + 5) / 10); }

} // namespace

// ─── Init / lifecycle ────────────────────────────────────────────────────────

CHIP_ERROR AirConditionerManager::Init(S21Manager& s21Manager, sync::SyncStack& syncStack)
{
    mS21Manager = &s21Manager;
    mSyncStack  = &syncStack;

    ReturnErrorOnFailure(InitLed());

    k_work_queue_start(&mS21WorkQueue, sS21WorkQueueStack,
                       K_THREAD_STACK_SIZEOF(sS21WorkQueueStack),
                       K_PRIO_PREEMPT(5), NULL);
    k_work_init_delayable(&mPollWork,      PollWorkHandler);
    k_work_init_delayable(&mInitRetryWork, InitRetryWorkHandler);
    k_work_init_delayable(&mCommandWork,   CommandWorkHandler);

    // Wire up SyncStack so AAI Writes and Polls schedule the right
    // follow-up work. Both hooks are static fns dispatching on Instance().
    mSyncStack->SetCommandPumpHook(&AirConditionerManager::SchedulePumpHook);
    mSyncStack->SetDirtyReporterHook(&AirConditionerManager::ReportDirtyAttributesHook);

    if (mS21Manager->Init() == 0) {
        k_work_reschedule_for_queue(&mS21WorkQueue, &mPollWork, K_NO_WAIT);
    } else {
        LOG_DBG("S21Manager Init failed; retry in %d ms", mInitRetryIntervalMs);
        k_work_reschedule_for_queue(&mS21WorkQueue, &mInitRetryWork,
                                    K_MSEC(mInitRetryIntervalMs));
    }

    // Sync the LED to current OnOff (boot-time read via SyncStack which
    // returns the boot defaults until the first poll lands).
    UpdatePowerIndicator(mSyncStack->ReadOnOff());

    LOG_DBG("AirConditionerManager initialised");
    return CHIP_NO_ERROR;
}

CHIP_ERROR AirConditionerManager::InitLed()
{
    if (!gpio_is_ready_dt(&led0)) {
        LOG_ERR("LED0 GPIO device is not ready");
        return CHIP_ERROR_INTERNAL;
    }
    int ret = gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure LED0 GPIO pin, error: %d", ret);
        return CHIP_ERROR_INTERNAL;
    }
    return CHIP_NO_ERROR;
}

void AirConditionerManager::UpdatePowerIndicator(bool onOff)
{
    gpio_pin_set_dt(&led0, onOff);
}

// ─── Poll loop (S21 work queue) ──────────────────────────────────────────────

void AirConditionerManager::PollWorkHandler(k_work* work)
{
    auto* dwork = k_work_delayable_from_work(work);
    auto& self  = *CONTAINER_OF(dwork, AirConditionerManager, mPollWork);
    self.Poll();
    k_work_reschedule_for_queue(&self.mS21WorkQueue, &self.mPollWork,
                                K_SECONDS(kS21PollIntervalSec));
}

void AirConditionerManager::Poll()
{
    auto op       = mS21Manager->getOperation();
    auto indoor   = mS21Manager->getRoomTemperature();
    auto outdoor  = mS21Manager->getOutdoorTemperature();
    auto humidity = mS21Manager->getHumidity();

    if (!op)       LOG_WRN("getOperation failed: %s",          op.error().message);
    if (!indoor)   LOG_WRN("getRoomTemperature failed: %s",    indoor.error().message);
    if (!outdoor)  LOG_INF("getOutdoorTemperature failed: %s", outdoor.error().message);
    if (!humidity) LOG_INF("getHumidity failed: %s",           humidity.error().message);

    // Reachable supervision: count consecutive whole-poll failures. The
    // primary getters are op/indoor; outdoor and humidity are optional on
    // some hardware variants so they don't contribute to the failure count.
    if (!op && !indoor) {
        if (++mConsecutivePollFailures >= kReachableFailureThreshold) {
            LOG_WRN("S21 link unresponsive after %d polls; marking unreachable",
                    mConsecutivePollFailures);
            mSyncStack->NotifyLinkDown();
        }
        return;
    }
    mConsecutivePollFailures = 0;

    if (!op || !indoor || !outdoor || !humidity) return;

    auto [onOff, mode, setpoint, fanMode] = *op;
    const S21State state{
        .onOff                         = onOff,
        .operatingMode                 = mode,
        .setpointCelsius               = setpoint,
        .fanMode                       = fanMode,
        .indoorTemperatureCelsius      = *indoor,
        .outdoorTemperatureCelsius     = *outdoor,
        .indoorRelativeHumidityPercent = *humidity,
    };

    mSyncStack->ApplyObservation(state);
    // Dirty-attribute flush, command-pump scheduling, AND the LED side
    // effect for OnOff transitions are all handled by the SyncStack
    // hooks installed in Init(). Nothing more to do here.
}

void AirConditionerManager::InitRetryWorkHandler(k_work* work)
{
    auto* dwork = k_work_delayable_from_work(work);
    auto& self  = *CONTAINER_OF(dwork, AirConditionerManager, mInitRetryWork);

    LOG_DBG("Retrying S21Manager::Init()");
    int err = self.mS21Manager->Init();
    LOG_DBG("S21Manager::Init() returned %d, isReady=%s", err,
            self.mS21Manager->isReady() ? "true" : "false");

    if (err == 0) {
        __ASSERT(self.mS21Manager->isReady(),
                 "S21Manager::Init() succeeded but manager is not ready");
        k_work_reschedule_for_queue(&self.mS21WorkQueue, &self.mPollWork, K_NO_WAIT);
    } else {
        k_work_reschedule_for_queue(&self.mS21WorkQueue, &self.mInitRetryWork,
                                    K_MSEC(self.mInitRetryIntervalMs));
        self.mInitRetryIntervalMs = MIN(self.mInitRetryIntervalMs * 2,
                                        kS21InitRetryMaximumIntervalMilliSec);
    }
}

// ─── Command pump (S21 work queue) ───────────────────────────────────────────

void AirConditionerManager::SchedulePumpHook()
{
    auto& self = Instance();
    k_work_reschedule_for_queue(&self.mS21WorkQueue, &self.mCommandWork,
                                K_MSEC(kCommandDebounceMs));
}

void AirConditionerManager::CommandWorkHandler(k_work* work)
{
    auto* dwork = k_work_delayable_from_work(work);
    auto& self  = *CONTAINER_OF(dwork, AirConditionerManager, mCommandWork);

    auto cmd = self.mSyncStack->PendingCommand();
    if (!cmd.has_value()) return;

    LOG_INF("Sending setOperation(onOff=%s, mode=%u, setpoint=%d, fan=%u) to S21",
            cmd->onOff ? "true" : "false",
            static_cast<uint8_t>(cmd->operatingMode),
            cmd->setpointCelsius,
            static_cast<uint8_t>(cmd->fanMode));

    auto result = self.mS21Manager->setOperation(
        cmd->onOff, cmd->operatingMode, cmd->setpointCelsius, cmd->fanMode);
    if (result) {
        self.mSyncStack->OnCommandSent(*cmd);
    } else {
        LOG_WRN("setOperation failed: %s", result.error().message);
        self.mSyncStack->OnCommandFailed();
    }
}

// ─── Dirty-attribute report flush (Matter event loop) ────────────────────────
//
// Nrf::PostTask only accepts trivially-copyable functors (no std::vector
// captures). We bridge with a tiny mutex-guarded queue: the hook appends
// paths and schedules a no-capture lambda that drains the queue on the
// Matter event loop under the CHIP stack lock. Both threads (Matter
// event loop and S21 work queue) can enqueue safely.

namespace {

K_MUTEX_DEFINE(sDirtyQueueMutex);
std::vector<sync::MatterAttributePath> sDirtyQueue;

void DrainDirtyQueue()
{
    std::vector<sync::MatterAttributePath> local;
    k_mutex_lock(&sDirtyQueueMutex, K_FOREVER);
    local.swap(sDirtyQueue);
    k_mutex_unlock(&sDirtyQueueMutex);
    if (local.empty()) return;

    PlatformMgr().LockChipStack();
    for (const auto& p : local) {
        MatterReportingAttributeChangeCallback(p.endpoint, p.cluster, p.attribute);
    }
    PlatformMgr().UnlockChipStack();
}

} // namespace

void AirConditionerManager::ReportDirtyAttributesHook(
    const std::vector<sync::MatterAttributePath>& paths)
{
    k_mutex_lock(&sDirtyQueueMutex, K_FOREVER);
    sDirtyQueue.insert(sDirtyQueue.end(), paths.begin(), paths.end());
    k_mutex_unlock(&sDirtyQueueMutex);

    Nrf::PostTask([] { DrainDirtyQueue(); }); // trivially-copyable: no captures

    // OnOff side effect: keep LED0 in sync with the bridge's view of power
    // state on every transition, whether the change came from a controller
    // (AAI Write) or a poll observation. Read the projected value rather
    // than the raw write payload so we always show what controllers see.
    for (const auto& p : paths) {
        if (p.cluster   == chip::app::Clusters::OnOff::Id &&
            p.attribute == chip::app::Clusters::OnOff::Attributes::OnOff::Id) {
            auto& self = Instance();
            self.UpdatePowerIndicator(self.mSyncStack->ReadOnOff());
            break;
        }
    }
}

// ─── Cached sensor reads (used by external thermostat readback) ──────────────

chip::app::DataModel::Nullable<int16_t> AirConditionerManager::GetLocalTemp()
{
    return mSyncStack->ReadLocalTemperature();
}

chip::app::DataModel::Nullable<int16_t> AirConditionerManager::GetOutdoorTemp()
{
    return mSyncStack->ReadOutdoorTemperature();
}

// ─── Debug logging (Button 2) ────────────────────────────────────────────────

const char* AirConditionerManager::GetSystemModeStr(
    Thermostat::SystemModeEnum mode)
{
    switch (mode) {
    case Thermostat::SystemModeEnum::kOff:           return "Off";
    case Thermostat::SystemModeEnum::kAuto:          return "Auto";
    case Thermostat::SystemModeEnum::kCool:          return "Cool";
    case Thermostat::SystemModeEnum::kHeat:          return "Heat";
    case Thermostat::SystemModeEnum::kEmergencyHeat: return "Emergency Heat";
    case Thermostat::SystemModeEnum::kPrecooling:    return "Precooling";
    case Thermostat::SystemModeEnum::kFanOnly:       return "Fan Only";
    case Thermostat::SystemModeEnum::kDry:           return "Dry";
    case Thermostat::SystemModeEnum::kSleep:         return "Sleep";
    default:                                         return "Unknown";
    }
}

const char* AirConditionerManager::GetRunningModeStr(
    Thermostat::ThermostatRunningModeEnum mode)
{
    switch (mode) {
    case Thermostat::ThermostatRunningModeEnum::kOff:  return "Off";
    case Thermostat::ThermostatRunningModeEnum::kCool: return "Cool";
    case Thermostat::ThermostatRunningModeEnum::kHeat: return "Heat";
    default:                                           return "Unknown";
    }
}

void AirConditionerManager::LogThermostatStatus()
{
    auto& s = *mSyncStack;
    LOG_INF("Thermostat (bridge view):");
    LOG_INF("  Mode - %s",       GetSystemModeStr(s.ReadSystemMode()));
    auto local = s.ReadLocalTemperature();
    auto outdoor = s.ReadOutdoorTemperature();
    if (!local.IsNull())
        LOG_INF("  LocalTemperature   - %d,%d'C",
                ReturnCompleteValue(local.Value()), ReturnRemainderValue(local.Value()));
    else
        LOG_INF("  LocalTemperature   - No Value");
    if (!outdoor.IsNull())
        LOG_INF("  OutdoorTemperature - %d,%d'C",
                ReturnCompleteValue(outdoor.Value()), ReturnRemainderValue(outdoor.Value()));
    else
        LOG_INF("  OutdoorTemperature - No Value");
    const int16_t heat = s.ReadOccupiedHeatingSetpoint();
    const int16_t cool = s.ReadOccupiedCoolingSetpoint();
    LOG_INF("  HeatingSetpoint    - %d,%d'C",
            ReturnCompleteValue(heat), ReturnRemainderValue(heat));
    LOG_INF("  CoolingSetpoint    - %d,%d'C",
            ReturnCompleteValue(cool), ReturnRemainderValue(cool));
}

void AirConditionerManager::LogMatterThermostatStatus()
{
    // Pre-cutover this dumped the cluster-server's RAM view; now the
    // bridge IS the cluster's view, so it's the same readout. Kept as a
    // separate function for compatibility with app_task.cpp's button.
    LogThermostatStatus();
}
