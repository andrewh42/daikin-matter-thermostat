/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "airconditioner_manager.h"
#include "matter_to_s21_translator.h"
#include "app/task_executor.h"
#include <app-common/zap-generated/cluster-objects.h>
#include <platform/PlatformManager.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <optional>
#include <atomic>

LOG_MODULE_REGISTER(aircon_mgr, LOG_LEVEL_DBG);

using namespace chip;
using namespace ::chip::DeviceLayer;
using namespace ::chip::app::Clusters;
using namespace chip::app::Clusters::FanControl::Attributes;
using namespace chip::app::Clusters::RelativeHumidityMeasurement::Attributes;
using namespace chip::app::Clusters::Thermostat::Attributes;
using namespace Protocols::InteractionModel;

K_THREAD_STACK_DEFINE(sS21WorkQueueStack, 2048);

namespace {
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* Convert and return only complete part of value to printable type */
uint8_t ReturnCompleteValue(int16_t Value)
{
    return static_cast<uint8_t>(Value / 100);
}

/* Converts and returns only reminder part of value to printable type.
 * This formula rounds reminder value to one significant figure
 */
uint8_t ReturnRemainderValue(int16_t Value)
{
    return static_cast<uint8_t>((Value % 100 + 5) / 10);
}

/* Check a Matter attribute Set() return value and log on failure. */
#define SET_AND_LOG(attr_call)                                              \
    do {                                                                    \
        auto _status = (attr_call);                                         \
        if (_status != Status::Success)                                     \
            LOG_ERR(#attr_call " failed: 0x%x", to_underlying(_status));    \
    } while (0)

} /* namespace */

// ─── ClusterMatterSink ────────────────────────────────────────────────────────

void AirConditionerManager::ClusterMatterSink::setOnOff(bool v)
{
    SET_AND_LOG(OnOff::Attributes::OnOff::Set(mEndpointId, v));
}

void AirConditionerManager::ClusterMatterSink::setSystemMode(
    chip::app::Clusters::Thermostat::SystemModeEnum m)
{
    SET_AND_LOG(SystemMode::Set(mEndpointId, m));
}

void AirConditionerManager::ClusterMatterSink::setRunningMode(
    chip::app::Clusters::Thermostat::ThermostatRunningModeEnum m)
{
    SET_AND_LOG(ThermostatRunningMode::Set(mEndpointId, m));
}

void AirConditionerManager::ClusterMatterSink::setCoolingSetpoint(int16_t v)
{
    SET_AND_LOG(OccupiedCoolingSetpoint::Set(mEndpointId, v));
}

void AirConditionerManager::ClusterMatterSink::setHeatingSetpoint(int16_t v)
{
    SET_AND_LOG(OccupiedHeatingSetpoint::Set(mEndpointId, v));
}

void AirConditionerManager::ClusterMatterSink::setFanSpeedSetting(std::optional<uint8_t> s)
{
    using namespace chip::app::Clusters::FanControl;
    if (s) {
        SET_AND_LOG(Attributes::SpeedSetting::Set(mEndpointId, DataModel::MakeNullable(*s)));
    } else {
        // Auto: write FanMode=kAuto and let the cluster server set SpeedSetting to null.
        // Writing null directly to SpeedSetting is rejected (InvalidInState) by the
        // fan-control-server unless the write originates from cluster logic.
        SET_AND_LOG(Attributes::FanMode::Set(mEndpointId, FanModeEnum::kAuto));
        SET_AND_LOG(Attributes::SpeedCurrent::Set(mEndpointId, 3)); // indicate a mid-range speed while in auto mode
    }
}

void AirConditionerManager::ClusterMatterSink::setLocalTemperature(int16_t v)
{
    SET_AND_LOG(LocalTemperature::Set(mEndpointId, v));
}

void AirConditionerManager::ClusterMatterSink::setOutdoorTemperature(int16_t v)
{
    SET_AND_LOG(OutdoorTemperature::Set(mEndpointId, v));
}

void AirConditionerManager::ClusterMatterSink::setHumidity(uint16_t v)
{
    SET_AND_LOG(MeasuredValue::Set(mEndpointId, v));
}

// ─── AirConditionerManager ────────────────────────────────────────────────────

CHIP_ERROR AirConditionerManager::Init(S21Manager& s21Manager)
{
    mS21Manager = &s21Manager;

    ReturnErrorOnFailure(InitLed());

    CHIP_ERROR err = CHIP_NO_ERROR;

    if (err != CHIP_NO_ERROR) {
        LOG_DBG("AirConditionerManager initialisation FAILED");
        return err;
    }

    k_work_queue_start(&mS21WorkQueue, sS21WorkQueueStack, K_THREAD_STACK_SIZEOF(sS21WorkQueueStack),
                   K_PRIO_PREEMPT(5), NULL);
    k_work_init_delayable(&mPollWork, PollWorkHandler);
    k_work_init_delayable(&mInitRetryWork, InitRetryWorkHandler);
    k_work_init(&mCommandWork, CommandWorkHandler);

    if (mS21Manager->Init() == 0) {
        k_work_reschedule_for_queue(&mS21WorkQueue, &mPollWork, K_NO_WAIT);
    }
    else {
        LOG_DBG("S21Manager initialisation failed, will retry initialisation in %d ms", mInitRetryIntervalMs);
        k_work_reschedule_for_queue(&mS21WorkQueue, &mInitRetryWork, K_MSEC(mInitRetryIntervalMs));
    }

    {
        bool onOff = false;
        PlatformMgr().LockChipStack();
        OnOff::Attributes::OnOff::Get(kThermostatEndpoint, &onOff);
        PlatformMgr().UnlockChipStack();
        UpdatePowerIndicator(onOff);
    }
    LOG_DBG("AirConditionerManager initialised successfully");
    return CHIP_NO_ERROR;
}

void AirConditionerManager::PollWorkHandler(k_work* work)
{
    auto* dwork = k_work_delayable_from_work(work);
    auto& self  = *CONTAINER_OF(dwork, AirConditionerManager, mPollWork);

    self.Poll();

    k_work_reschedule_for_queue(&self.mS21WorkQueue, &self.mPollWork, K_SECONDS(kS21PollIntervalSec));
}

void AirConditionerManager::Poll()
{
    if (mPendingCommandFlags.load() & kCommandOperation) {
        LOG_DBG("S21 command pending, skipping poll");
        return;
    }

    auto op       = mS21Manager->getOperation();
    auto indoor   = mS21Manager->getRoomTemperature();
    auto outdoor  = mS21Manager->getOutdoorTemperature();
    auto humidity = mS21Manager->getHumidity();

    if (!op)       LOG_WRN("getOperation failed: %s",          op.error().message);
    if (!indoor)   LOG_WRN("getRoomTemperature failed: %s",    indoor.error().message);
    if (!outdoor)  LOG_INF("getOutdoorTemperature failed: %s", outdoor.error().message);
    if (!humidity) LOG_INF("getHumidity failed: %s",           humidity.error().message);

    if (!op || !indoor || !outdoor || !humidity) return;

    auto [onOff, mode, setpoint, fanMode] = *op;
    S21State state{
        .onOff                         = onOff,
        .operatingMode                 = mode,
        .setpointCelsius               = setpoint,
        .fanMode                       = fanMode,
        .indoorTemperatureCelsius      = *indoor,
        .outdoorTemperatureCelsius     = *outdoor,
        .indoorRelativeHumidityPercent = *humidity,
    };

    Nrf::PostTask([this, state] {
        PlatformMgr().LockChipStack();
        mUpdatingFromPoll = true;
        S21ToMatterTranslator(state, mMatterSink).translate();
        mUpdatingFromPoll = false;
        PlatformMgr().UnlockChipStack();
    });
}

void AirConditionerManager::InitRetryWorkHandler(k_work* work)
{
    auto* dwork = k_work_delayable_from_work(work);
    auto& self  = *CONTAINER_OF(dwork, AirConditionerManager, mInitRetryWork);

    LOG_DBG("Retrying S21Manager::Init()");
    int err = self.mS21Manager->Init();
    LOG_DBG("S21Manager::Init() returned %d, isReady=%s", err, self.mS21Manager->isReady() ? "true" : "false");

    if (err == 0) {
        __ASSERT(self.mS21Manager->isReady(), "S21Manager::Init() succeeded but manager is not ready");
        LOG_DBG("S21Manager is ready, starting polling work");
        k_work_reschedule_for_queue(&self.mS21WorkQueue, &self.mPollWork, K_NO_WAIT);
    }
    else {
        LOG_DBG("S21Manager initialisation failed, will retry in %d ms", self.mInitRetryIntervalMs);
        k_work_reschedule_for_queue(&self.mS21WorkQueue, &self.mInitRetryWork, K_MSEC(self.mInitRetryIntervalMs));
        self.mInitRetryIntervalMs = MIN(self.mInitRetryIntervalMs * 2, kS21InitRetryMaximumIntervalMilliSec);
    }
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

const char* AirConditionerManager::GetSystemModeStr(app::Clusters::Thermostat::SystemModeEnum mode)
{
    switch (mode) {
    case app::Clusters::Thermostat::SystemModeEnum::kOff:
        return "Off";
    case app::Clusters::Thermostat::SystemModeEnum::kAuto:
        return "Auto";
    case app::Clusters::Thermostat::SystemModeEnum::kCool:
        return "Cool";
    case app::Clusters::Thermostat::SystemModeEnum::kHeat:
        return "Heat";
    case app::Clusters::Thermostat::SystemModeEnum::kEmergencyHeat:
        return "Emergency Heat";
    case app::Clusters::Thermostat::SystemModeEnum::kPrecooling:
        return "Precooling";
    case app::Clusters::Thermostat::SystemModeEnum::kFanOnly:
        return "Fan Only";
    case app::Clusters::Thermostat::SystemModeEnum::kDry:
        return "Dry";
    case app::Clusters::Thermostat::SystemModeEnum::kSleep:
        return "Sleep";
    default:
        return "Unknown Mode";
    }
}

const char* AirConditionerManager::GetRunningModeStr(app::Clusters::Thermostat::ThermostatRunningModeEnum mode) {
    switch (mode) {
    case app::Clusters::Thermostat::ThermostatRunningModeEnum::kOff:
        return "Off";
    case app::Clusters::Thermostat::ThermostatRunningModeEnum::kCool:
        return "Cool";
    case app::Clusters::Thermostat::ThermostatRunningModeEnum::kHeat:
        return "Heat";
    default:
        return "Unknown Mode";
    }
}

void AirConditionerManager::LogThermostatStatus()
{
    app::Clusters::Thermostat::SystemModeEnum systemMode;
    int16_t heatingSetpoint, coolingSetpoint;
    DataModel::Nullable<int16_t> localTemp, outdoorTemp;
    PlatformMgr().LockChipStack();
    SystemMode::Get(kThermostatEndpoint, &systemMode);
    OccupiedHeatingSetpoint::Get(kThermostatEndpoint, &heatingSetpoint);
    OccupiedCoolingSetpoint::Get(kThermostatEndpoint, &coolingSetpoint);
    LocalTemperature::Get(kThermostatEndpoint, localTemp);
    OutdoorTemperature::Get(kThermostatEndpoint, outdoorTemp);
    PlatformMgr().UnlockChipStack();

    LOG_INF("Thermostat:");
    LOG_INF("	Mode - %s", GetSystemModeStr(systemMode));
    if (!localTemp.IsNull()) {
        int16_t tempValue = localTemp.Value();
        LOG_INF("	LocalTemperature - %d,%d'C", ReturnCompleteValue(tempValue), ReturnRemainderValue(tempValue));
    }
    else {
        LOG_INF("	LocalTemperature - No Value");
    }
    if (!outdoorTemp.IsNull()) {
        int16_t tempValue = outdoorTemp.Value();
        LOG_INF("	OutdoorTemperature - %d,%d'C", ReturnCompleteValue(tempValue), ReturnRemainderValue(tempValue));
    }
    else {
        LOG_INF("	OutdoorTemperature - No Value");
    }
    LOG_INF("	HeatingSetpoint - %d,%d'C", ReturnCompleteValue(heatingSetpoint),
            ReturnRemainderValue(heatingSetpoint));
    LOG_INF("	CoolingSetpoint - %d,%d'C \n", ReturnCompleteValue(coolingSetpoint),
            ReturnRemainderValue(coolingSetpoint));
}

void AirConditionerManager::LogMatterThermostatStatus()
{
    using Status = Protocols::InteractionModel::Status;

    app::Clusters::Thermostat::SystemModeEnum systemMode;
    app::Clusters::Thermostat::ThermostatRunningModeEnum runningMode;
    DataModel::Nullable<int16_t> localTemp, outdoorTemp, coilTemp;
    int16_t coolingSetpoint, heatingSetpoint;
    int8_t deadBand;
    chip::BitMask<app::Clusters::Thermostat::HVACSystemTypeBitmap> hvacType;
    app::Clusters::Thermostat::ControlSequenceOfOperationEnum ctrlSeq;

    PlatformMgr().LockChipStack();
    Status sSystemMode   = SystemMode::Get(kThermostatEndpoint, &systemMode);
    Status sRunningMode  = ThermostatRunningMode::Get(kThermostatEndpoint, &runningMode);
    Status sLocalTemp    = LocalTemperature::Get(kThermostatEndpoint, localTemp);
    Status sOutdoorTemp  = OutdoorTemperature::Get(kThermostatEndpoint, outdoorTemp);
    Status sCoilTemp     = ACCoilTemperature::Get(kThermostatEndpoint, coilTemp);
    Status sCooling      = OccupiedCoolingSetpoint::Get(kThermostatEndpoint, &coolingSetpoint);
    Status sHeating      = OccupiedHeatingSetpoint::Get(kThermostatEndpoint, &heatingSetpoint);
    Status sDeadBand     = MinSetpointDeadBand::Get(kThermostatEndpoint, &deadBand);
    Status sHvacType     = HVACSystemTypeConfiguration::Get(kThermostatEndpoint, &hvacType);
    Status sCtrlSeq      = ControlSequenceOfOperation::Get(kThermostatEndpoint, &ctrlSeq);
    PlatformMgr().UnlockChipStack();

    LOG_INF("Matter Thermostat Cluster State:");

    if (sSystemMode == Status::Success) {
        LOG_INF("	System Mode - %s", GetSystemModeStr(systemMode));
    } else {
        LOG_INF("	System Mode - Error: %d", static_cast<uint8_t>(sSystemMode));
    }

    if (sRunningMode == Status::Success) {
        LOG_INF("	Running Mode - %s", GetRunningModeStr(runningMode));
    } else {
        LOG_INF("	Running Mode - Error: %d", static_cast<uint8_t>(sRunningMode));
    }

    if (sLocalTemp == Status::Success) {
        if (!localTemp.IsNull()) {
            LOG_INF("	LocalTemperature - %d.%d'C", ReturnCompleteValue(localTemp.Value()), ReturnRemainderValue(localTemp.Value()));
        } else {
            LOG_INF("	LocalTemperature - No Value");
        }
    } else {
        LOG_INF("	LocalTemperature - Error: %d", static_cast<uint8_t>(sLocalTemp));
    }

    if (sOutdoorTemp == Status::Success) {
        if (!outdoorTemp.IsNull()) {
            LOG_INF("	OutdoorTemperature - %d.%d'C", ReturnCompleteValue(outdoorTemp.Value()), ReturnRemainderValue(outdoorTemp.Value()));
        } else {
            LOG_INF("	OutdoorTemperature - No Value");
        }
    } else {
        LOG_INF("	OutdoorTemperature - Error: %d", static_cast<uint8_t>(sOutdoorTemp));
    }

    if (sCoilTemp == Status::Success) {
        if (!coilTemp.IsNull()) {
            LOG_INF("	ACCoilTemperature - %d.%d'C", ReturnCompleteValue(coilTemp.Value()), ReturnRemainderValue(coilTemp.Value()));
        } else {
            LOG_INF("	ACCoilTemperature - No Value");
        }
    } else {
        LOG_INF("	ACCoilTemperature - Error: %d", static_cast<uint8_t>(sCoilTemp));
    }

    if (sCooling == Status::Success) {
        LOG_INF("	OccupiedCoolingSetpoint - %d.%d'C", ReturnCompleteValue(coolingSetpoint), ReturnRemainderValue(coolingSetpoint));
    } else {
        LOG_INF("	OccupiedCoolingSetpoint - Error: %d", static_cast<uint8_t>(sCooling));
    }

    if (sHeating == Status::Success) {
        LOG_INF("	OccupiedHeatingSetpoint - %d.%d'C", ReturnCompleteValue(heatingSetpoint), ReturnRemainderValue(heatingSetpoint));
    } else {
        LOG_INF("	OccupiedHeatingSetpoint - Error: %d", static_cast<uint8_t>(sHeating));
    }

    if (sDeadBand == Status::Success) {
        LOG_INF("	MinSetpointDeadBand - %d.%d'C", deadBand / 10, deadBand % 10);
    } else {
        LOG_INF("	MinSetpointDeadBand - Error: %d", static_cast<uint8_t>(sDeadBand));
    }

    if (sHvacType == Status::Success) {
        LOG_INF("	HVACSystemTypeConfiguration - 0x%02x", hvacType.Raw());
    } else {
        LOG_INF("	HVACSystemTypeConfiguration - Error: %d", static_cast<uint8_t>(sHvacType));
    }

    if (sCtrlSeq == Status::Success) {
        LOG_INF("	ControlSequenceOfOperation - %u", static_cast<uint8_t>(ctrlSeq));
    } else {
        LOG_INF("	ControlSequenceOfOperation - Error: %d", static_cast<uint8_t>(sCtrlSeq));
    }
}

void AirConditionerManager::AttributeChangeHandler(const ConcreteAttributePath& attributePath, uint8_t* value,
                                                   uint16_t size)
{
    switch (attributePath.mClusterId) {
    case FanControl::Id:
        FanControlAttributeChangeHandler(attributePath.mAttributeId, value, size);
        break;
    case Thermostat::Id:
        TemperatureAttributeChangeHandler(attributePath.mAttributeId, value, size);
        break;
    case OnOff::Id:
        OnOffAttributeChangeHandler(attributePath.mAttributeId, value, size);
        break;
    case RelativeHumidityMeasurement::Id:
        // RelativeHumidityMeasurement cluster is read-only so we only log changes for debugging purposes
        LOG_INF("RelativeHumidityMeasurement cluster attribute changed: Attribute ID %u, Value: %u, Size: %u",
                attributePath.mAttributeId, *value, size);
        break;
    default:
        LOG_INF("Unhandled cluster ID: %u", attributePath.mClusterId);
    }
}

void AirConditionerManager::OnOffAttributeChangeHandler(AttributeId attributeId, uint8_t* value, uint16_t size)
{
    bool newOnOff = *value;
    UpdatePowerIndicator(newOnOff);
    if (mUpdatingFromPoll) return;
    LOG_INF("Cluster OnOff: attribute OnOff set to %s", newOnOff ? "ON" : "OFF");
    mPendingCommandFlags.fetch_or(kCommandOperation);
    k_work_submit_to_queue(&mS21WorkQueue, &mCommandWork);
}

void AirConditionerManager::TemperatureAttributeChangeHandler(AttributeId attributeId, uint8_t* value, uint16_t size)
{
    switch (attributeId) {
    case LocalTemperature::Id:
    case OutdoorTemperature::Id:
        break; // updated by Poll() via ClusterMatterSink

    case OccupiedCoolingSetpoint::Id: {
        auto coolingSetpoint = *reinterpret_cast<int16_t*>(value);
        if (!mUpdatingFromPoll) {
            LOG_INF("Cooling TEMP -> %d", coolingSetpoint);
            app::Clusters::Thermostat::SystemModeEnum currentMode;
            SystemMode::Get(kThermostatEndpoint, &currentMode);
            if (currentMode == app::Clusters::Thermostat::SystemModeEnum::kCool ||
                currentMode == app::Clusters::Thermostat::SystemModeEnum::kAuto) {
                mPendingCommandFlags.fetch_or(kCommandOperation);
                k_work_submit_to_queue(&mS21WorkQueue, &mCommandWork);
            }
        }
    } break;

    case OccupiedHeatingSetpoint::Id: {
        auto heatingSetpoint = *reinterpret_cast<int16_t*>(value);
        if (!mUpdatingFromPoll) {
            LOG_INF("Heating TEMP -> %d", heatingSetpoint);
            app::Clusters::Thermostat::SystemModeEnum currentMode;
            SystemMode::Get(kThermostatEndpoint, &currentMode);
            if (currentMode == app::Clusters::Thermostat::SystemModeEnum::kHeat ||
                currentMode == app::Clusters::Thermostat::SystemModeEnum::kAuto) {
                mPendingCommandFlags.fetch_or(kCommandOperation);
                k_work_submit_to_queue(&mS21WorkQueue, &mCommandWork);
            }
        }
    } break;

    case SystemMode::Id: {
        auto systemMode = static_cast<app::Clusters::Thermostat::SystemModeEnum>(*value);
        if (!mUpdatingFromPoll) {
            LOG_INF("System Mode changed -> %s", GetSystemModeStr(systemMode));
            if (systemMode != app::Clusters::Thermostat::SystemModeEnum::kOff) {
                mPendingCommandFlags.fetch_or(kCommandOperation);
                k_work_submit_to_queue(&mS21WorkQueue, &mCommandWork);
            }
        }
    } break;

    case ThermostatRunningMode::Id:
        LOG_INF("ThermostatRunningMode changed to %u", *value);
        break;
    default: {
        LOG_ERR("Unhandled thermostat attribute %x", attributeId);
        return;
    } break;
    }

    LogThermostatStatus();
    LogMatterThermostatStatus();
}

void AirConditionerManager::FanControlAttributeChangeHandler(AttributeId attributeId, uint8_t* value,
                                                              uint16_t size)
{
    using namespace chip::app::Clusters::FanControl;

    switch (attributeId) {
    case Attributes::SpeedSetting::Id: {
        if (!mUpdatingFromPoll) {
            LOG_DBG("SpeedSetting changed -> %u", *value);
            mPendingCommandFlags.fetch_or(kCommandOperation);
            k_work_submit_to_queue(&mS21WorkQueue, &mCommandWork);
        }
        break;
    }
    case Attributes::FanMode::Id: {
        auto fanModeEnum = static_cast<FanModeEnum>(*value);
        if ((fanModeEnum == FanModeEnum::kAuto || fanModeEnum == FanModeEnum::kSmart) && !mUpdatingFromPoll) {
            // SpeedSetting may already be null (no SpeedSetting callback will fire), so queue here.
            LOG_DBG("FanMode -> Auto");
            mPendingCommandFlags.fetch_or(kCommandOperation);
            k_work_submit_to_queue(&mS21WorkQueue, &mCommandWork);
        }
        break;
    }
    default:
        LOG_DBG("FanControl cluster attribute %u changed: value %u", attributeId, *value);
        break;
    }
}

void AirConditionerManager::CommandWorkHandler(k_work* work)
{
    auto& self = *CONTAINER_OF(work, AirConditionerManager, mCommandWork);
    self.ExecutePendingCommands();
}

void AirConditionerManager::ExecutePendingCommands()
{
    uint32_t flags = mPendingCommandFlags.exchange(0);

    if (flags & kCommandOperation) {
        MatterState state{};
        {
            using namespace chip::app::Clusters::FanControl;
            PlatformMgr().LockChipStack();
            OnOff::Attributes::OnOff::Get(kThermostatEndpoint, &state.onOff);
            SystemMode::Get(kThermostatEndpoint, &state.systemMode);
            OccupiedCoolingSetpoint::Get(kThermostatEndpoint, &state.coolingSetpointCelsius);
            OccupiedHeatingSetpoint::Get(kThermostatEndpoint, &state.heatingSetpointCelsius);
            Attributes::SpeedSetting::Get(kThermostatEndpoint, state.speedSetting);
            PlatformMgr().UnlockChipStack();
        }

        auto cmd = MatterToS21Translator::translate(state);

        if (mLastSentCommand == cmd) {
            LOG_DBG("setOperation: no change, skipping S21 command");
            return;
        }

        LOG_INF("Sending setOperation(onOff=%s, mode=%u, setpoint=%d, fan=%u) to S21",
                cmd.onOff ? "true" : "false", static_cast<uint8_t>(cmd.operatingMode),
                cmd.setpointCelsius, static_cast<uint8_t>(cmd.fanMode));
        auto result = mS21Manager->setOperation(cmd.onOff, cmd.operatingMode, cmd.setpointCelsius, cmd.fanMode);
        if (!result) {
            LOG_WRN("setOperation failed: %s", result.error().message);
        } else {
            mLastSentCommand = cmd;
        }
    }
}


void AirConditionerManager::UpdatePowerIndicator(bool onOff)
{
    gpio_pin_set_dt(&led0, onOff);
}

app::DataModel::Nullable<int16_t> AirConditionerManager::GetLocalTemp()
{
    DataModel::Nullable<int16_t> v;
    PlatformMgr().LockChipStack();
    LocalTemperature::Get(kThermostatEndpoint, v);
    PlatformMgr().UnlockChipStack();
    return v;
}

app::DataModel::Nullable<int16_t> AirConditionerManager::GetOutdoorTemp()
{
    DataModel::Nullable<int16_t> v;
    PlatformMgr().LockChipStack();
    OutdoorTemperature::Get(kThermostatEndpoint, v);
    PlatformMgr().UnlockChipStack();
    return v;
}
