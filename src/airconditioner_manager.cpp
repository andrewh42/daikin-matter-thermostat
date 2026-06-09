/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "airconditioner_manager.h"
#include "sync/aai_translation.h"
#include "sync/s21_observation.h"
#include "sync/sync_coordinator.h"

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

uint8_t ReturnCompleteValue(int16_t v)  { return static_cast<uint8_t>(v / 100); }
uint8_t ReturnRemainderValue(int16_t v) { return static_cast<uint8_t>((v % 100 + 5) / 10); }

} // namespace

// ─── PowerIndicatorListener: LED0 ownership ─────────────────────────────────

const struct gpio_dt_spec
AirConditionerManager::PowerIndicator::sLed0 =
    GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

int AirConditionerManager::PowerIndicator::Init(bool initialOnOff)
{
    if (!gpio_is_ready_dt(&sLed0)) {
        LOG_ERR("LED0 GPIO device is not ready");
        return -ENODEV;
    }
    const int flags = initialOnOff ? GPIO_OUTPUT_ACTIVE : GPIO_OUTPUT_INACTIVE;
    const int rc = gpio_pin_configure_dt(&sLed0, flags);
    if (rc < 0) {
        LOG_ERR("Failed to configure LED0 GPIO pin (%d)", rc);
        return rc;
    }
    return 0;
}

void AirConditionerManager::PowerIndicator::Set(bool onOff)
{
    gpio_pin_set_dt(&sLed0, onOff);
}

void AirConditionerManager::PowerIndicator::OnChangedAttributes(
    const std::vector<sync::LogicalAttribute>& attributes)
{
    for (auto attr : attributes) {
        if (attr == sync::LogicalAttribute::OnOff) {
            // Both gpio_pin_set_dt and ReadOnOff are thread-safe and
            // don't touch the CHIP stack lock, so this runs synchronously
            // on whichever thread dispatched the listener.
            Set(mOwner.mSyncCoordinator->ReadOnOff());
            return;
        }
    }
}

// ─── MatterReportListener: dirty-attribute queue + drain ────────────────────

void AirConditionerManager::MatterAttributeChangeReporter::OnChangedAttributes(
    const std::vector<sync::LogicalAttribute>& attributes)
{
    k_mutex_lock(&mQueueMutex, K_FOREVER);
    mQueue.insert(mQueue.end(), attributes.begin(), attributes.end());
    k_mutex_unlock(&mQueueMutex);

    Nrf::PostTask([this] { DrainOnMatterEventLoop(); });
}

void AirConditionerManager::MatterAttributeChangeReporter::DrainOnMatterEventLoop()
{
    std::vector<sync::LogicalAttribute> local;
    k_mutex_lock(&mQueueMutex, K_FOREVER);
    local.swap(mQueue);
    k_mutex_unlock(&mQueueMutex);
    if (local.empty()) return;

    PlatformMgr().LockChipStack();
    for (auto attr : local) {
        const auto [cluster, attribute] = sync_aai::toMatterAddress(attr);
        // The {0,0} sentinel marks attributes without a Matter address yet
        // (currently only BridgedDeviceBasicInformation::Reachable, pending
        // Phase 8 ZAP work). Skip those rather than emitting a bogus report.
        if (cluster == 0 && attribute == 0) continue;
        MatterReportingAttributeChangeCallback(kThermostatEndpoint, cluster, attribute);
    }
    PlatformMgr().UnlockChipStack();
}

// ─── Init / lifecycle ────────────────────────────────────────────────────────

CHIP_ERROR AirConditionerManager::Init(S21Manager& s21Manager, sync::SyncCoordinator& syncStack)
{
    mS21Manager = &s21Manager;
    mSyncCoordinator  = &syncStack;

    // Configure LED0 and drive it to the current OnOff before registering
    // the listener — a dispatch arriving before Init returns would
    // otherwise fire gpio_pin_set_dt on an unconfigured pin.
    if (mPowerIndicator.Init(mSyncCoordinator->ReadOnOff()) != 0) {
        return CHIP_ERROR_INTERNAL;
    }

    k_work_queue_start(&mS21WorkQueue, sS21WorkQueueStack,
                       K_THREAD_STACK_SIZEOF(sS21WorkQueueStack),
                       K_PRIO_PREEMPT(5), NULL);
    k_work_init_delayable(&mPollWork,      PollWorkHandler);
    k_work_init_delayable(&mInitRetryWork, InitRetryWorkHandler);
    k_work_init_delayable(&mCommandWork,   CommandWorkHandler);

    // Wire up SyncCoordinator so AAI Writes and Polls schedule the right
    // follow-up work. The pump handler is a static fn; the two listeners
    // are nested-class members that reach mOwner for context.
    mSyncCoordinator->SetCommandPumpHandler(&AirConditionerManager::ScheduleS21CommandHandler);
    ReturnErrorOnFailure(mSyncCoordinator->AddChangedAttributesListener(&mMatterAttributeChangeReporter));
    ReturnErrorOnFailure(mSyncCoordinator->AddChangedAttributesListener(&mPowerIndicator));

    if (mS21Manager->Init() == 0) {
        k_work_reschedule_for_queue(&mS21WorkQueue, &mPollWork, K_NO_WAIT);
    } else {
        LOG_DBG("S21Manager Init failed; retry in %d ms", mInitRetryIntervalMs);
        k_work_reschedule_for_queue(&mS21WorkQueue, &mInitRetryWork,
                                    K_MSEC(mInitRetryIntervalMs));
    }

    LOG_DBG("AirConditionerManager initialised");
    return CHIP_NO_ERROR;
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
    // Env first: when both phases fire on the same tick, the operational
    // projector pass downstream sees the freshly-applied indoor temperature
    // when it computes RunningMode (Auto-mode disambiguation, no-valve
    // hysteresis fallback). Dirty attribution lands in one pass instead of
    // an env-then-op re-flip.
    PollEnvironmentalPhase();
    PollOperationalPhase();
    // Dirty-attribute flush, command-pump scheduling, and the LED side
    // effect for OnOff transitions are all handled by the SyncCoordinator hooks
    // installed in Init().
}

void AirConditionerManager::PollEnvironmentalPhase()
{
    const bool envTick = (mEnvironmentalReadCountdown == 0);
    if (envTick) mEnvironmentalReadCountdown = kS21EnvironmentalSensorReadTicks - 1;
    else         --mEnvironmentalReadCountdown;
    if (!envTick) return;

    auto indoor   = mS21Manager->getRoomTemperature();
    auto outdoor  = mS21Manager->getOutdoorTemperature();
    auto humidity = mS21Manager->getHumidity();

    if (!indoor)   LOG_WRN("getRoomTemperature failed: %s",    indoor.error().message);
    if (!outdoor)  LOG_INF("getOutdoorTemperature failed: %s", outdoor.error().message);
    if (!humidity) LOG_INF("getHumidity failed: %s",           humidity.error().message);

    // All-or-nothing env contract (C1 split). Partial sensor data would
    // leave LogicalACState's SensorFields out of sync with each other for
    // the rest of the env cycle; better to skip this tick and try the
    // next one ~2 minutes later.
    if (!indoor || !outdoor || !humidity) return;

    mSyncCoordinator->ApplyEnvironmentalObservation(sync::S21EnvironmentalObservation{
        .indoorTemperatureCelsius      = *indoor,
        .outdoorTemperatureCelsius     = *outdoor,
        .indoorRelativeHumidityPercent = *humidity,
    });
}

void AirConditionerManager::PollOperationalPhase()
{
    auto op = mS21Manager->getOperation();
    if (!op) LOG_WRN("getOperation failed: %s", op.error().message);

    // Reachability heartbeat is op alone: env doesn't run every tick.
    if (!op) {
        if (++mConsecutivePollFailures >= kReachableFailureThreshold) {
            LOG_WRN("S21 link unresponsive after %d polls; marking unreachable",
                    mConsecutivePollFailures);
            mSyncCoordinator->NotifyLinkDown();
        }
        return;
    }
    mConsecutivePollFailures = 0;

    auto [onOff, mode, setpoint, fanMode] = *op;

    // Only query RzB2 when the unit reports powered on: when off, the
    // refrigerant valve is closed and the projector already short-circuits
    // RunningMode to Off on !onOff, so the read is pure overhead.
    std::optional<bool> refrigerantValveOpen;
    if (onOff) {
        auto unitState = mS21Manager->getUnitState();
        if (!unitState) LOG_INF("getUnitState failed: %s", unitState.error().message);
        else            refrigerantValveOpen = unitState->refrigerantValveOpen;
    }

    mSyncCoordinator->ApplyOperationalObservation(sync::S21OperationalObservation{
        .onOff                = onOff,
        .operatingMode        = mode,
        .setpointCelsius      = setpoint,
        .fanMode              = fanMode,
        .refrigerantValveOpen = refrigerantValveOpen,
    });
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

void AirConditionerManager::ScheduleS21CommandHandler()
{
    auto& self = Instance();
    k_work_reschedule_for_queue(&self.mS21WorkQueue, &self.mCommandWork,
                                K_MSEC(kCommandDebounceMs));
}

void AirConditionerManager::CommandWorkHandler(k_work* work)
{
    auto* dwork = k_work_delayable_from_work(work);
    auto& self  = *CONTAINER_OF(dwork, AirConditionerManager, mCommandWork);

    auto cmd = self.mSyncCoordinator->PendingCommand();
    if (!cmd.has_value()) return;

    LOG_INF("Sending setOperation(onOff=%s, mode=%u, setpoint=%d, fan=%u) to S21",
            cmd->onOff ? "true" : "false",
            static_cast<uint8_t>(cmd->operatingMode),
            cmd->setpointCelsius,
            static_cast<uint8_t>(cmd->fanMode));

    auto result = self.mS21Manager->setOperation(
        cmd->onOff, cmd->operatingMode, cmd->setpointCelsius, cmd->fanMode);
    if (result) {
        self.mSyncCoordinator->OnCommandSent(*cmd);
    } else {
        LOG_WRN("setOperation failed: %s", result.error().message);
        self.mSyncCoordinator->OnCommandFailed();
    }
}

// ─── Cached sensor reads (used by external thermostat readback) ──────────────

std::optional<int16_t> AirConditionerManager::GetLocalTemp()
{
    return mSyncCoordinator->ReadLocalTemperature();
}

std::optional<int16_t> AirConditionerManager::GetOutdoorTemp()
{
    return mSyncCoordinator->ReadOutdoorTemperature();
}

// ─── Debug logging (Button 2) ────────────────────────────────────────────────

const char* AirConditionerManager::GetOperationalModeStr(sync::OperationalMode mode)
{
    switch (mode) {
    case sync::OperationalMode::Auto:    return "Auto";
    case sync::OperationalMode::Cool:    return "Cool";
    case sync::OperationalMode::Heat:    return "Heat";
    case sync::OperationalMode::FanOnly: return "Fan Only";
    case sync::OperationalMode::Dry:     return "Dry";
    }
    return "Unknown";
}

const char* AirConditionerManager::GetRunningModeStr(sync::RunningMode mode)
{
    switch (mode) {
    case sync::RunningMode::Off:     return "Off";
    case sync::RunningMode::Cooling: return "Cooling";
    case sync::RunningMode::Heating: return "Heating";
    }
    return "Unknown";
}

void AirConditionerManager::LogThermostatStatus()
{
    auto& r = *mSyncCoordinator;
    LOG_INF("Thermostat (bridge view):");
    LOG_INF("  Power - %s",      r.ReadOnOff() ? "On" : "Off");
    LOG_INF("  Mode  - %s",      GetOperationalModeStr(r.ReadMode()));
    LOG_INF("  Run   - %s",      GetRunningModeStr(r.ReadRunningMode()));
    auto local = r.ReadLocalTemperature();
    auto outdoor = r.ReadOutdoorTemperature();
    if (local.has_value())
        LOG_INF("  LocalTemperature   - %d,%d'C",
                ReturnCompleteValue(*local), ReturnRemainderValue(*local));
    else
        LOG_INF("  LocalTemperature   - No Value");
    if (outdoor.has_value())
        LOG_INF("  OutdoorTemperature - %d,%d'C",
                ReturnCompleteValue(*outdoor), ReturnRemainderValue(*outdoor));
    else
        LOG_INF("  OutdoorTemperature - No Value");
    const int16_t heat = r.ReadOccupiedHeatingSetpoint();
    const int16_t cool = r.ReadOccupiedCoolingSetpoint();
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
