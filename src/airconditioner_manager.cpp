/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "airconditioner_manager.h"
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

    PlatformMgr().LockChipStack();

    VerifyOrExit(Status::Success == OccupiedCoolingSetpoint::Get(kThermostatEndpoint, &mCoolingSetPointCelsius),
                 err = CHIP_IM_GLOBAL_STATUS(Failure));
    VerifyOrExit(Status::Success == OccupiedHeatingSetpoint::Get(kThermostatEndpoint, &mHeatingSetPointCelsius),
                 err = CHIP_IM_GLOBAL_STATUS(Failure));
    VerifyOrExit(Status::Success == SystemMode::Get(kThermostatEndpoint, &mSystemMode),
                 err = CHIP_IM_GLOBAL_STATUS(Failure));
    VerifyOrExit(Status::Success == OnOff::Attributes::OnOff::Get(kThermostatEndpoint, &mOnOff),
                    err = CHIP_IM_GLOBAL_STATUS(Failure));

    {
        using namespace chip::app::Clusters::FanControl;
        DataModel::Nullable<uint8_t> initSpeedSetting;
        VerifyOrExit(Status::Success == Attributes::SpeedSetting::Get(kThermostatEndpoint, initSpeedSetting),
                     err = CHIP_IM_GLOBAL_STATUS(Failure));
        if (initSpeedSetting.IsNull()) {
            mFanMode = ::FanMode::Auto;
        } else {
            auto mapped = SpeedSettingToS21FanMode(initSpeedSetting.Value());
            if (mapped) {
                mFanMode = *mapped;
            } else {
                LOG_WRN("Unsupported initial fan speed setting %d, defaulting to Auto", initSpeedSetting.Value());
                mFanMode = ::FanMode::Auto;
            }
        }
    }

exit:
    PlatformMgr().UnlockChipStack();

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

    UpdatePowerIndicator();
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
    LOG_INF("Thermostat:");
    LOG_INF("	Mode - %s", GetSystemModeStr(mSystemMode));
    auto localTemp = GetLocalTemp();
    if (!localTemp.IsNull()) {
        int16_t tempValue = localTemp.Value();
        LOG_INF("	LocalTemperature - %d,%d'C", ReturnCompleteValue(tempValue), ReturnRemainderValue(tempValue));
    }
    else {
        LOG_INF("	LocalTemperature - No Value");
    }
    auto outdoorTemp = GetOutdoorTemp();
    if (!outdoorTemp.IsNull()) {
        int16_t tempValue = outdoorTemp.Value();
        LOG_INF("	OutdoorTemperature - %d,%d'C", ReturnCompleteValue(tempValue), ReturnRemainderValue(tempValue));
    }
    else {
        LOG_INF("	OutdoorTemperature - No Value");
    }
    LOG_INF("	HeatingSetpoint - %d,%d'C", ReturnCompleteValue(mHeatingSetPointCelsius),
            ReturnRemainderValue(mHeatingSetPointCelsius));
    LOG_INF("	CoolingSetpoint - %d,%d'C \n", ReturnCompleteValue(mCoolingSetPointCelsius),
            ReturnRemainderValue(mCoolingSetPointCelsius));
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
    if (newOnOff == mOnOff) {
        LOG_DBG("OnOff: %s (unchanged)", mOnOff ? "ON" : "OFF");
        return;
    }
    mOnOff = newOnOff;
    UpdatePowerIndicator();
    if (mUpdatingFromPoll) return;
    LOG_INF("Cluster OnOff: attribute OnOff set to %s", mOnOff ? "ON" : "OFF");
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
        if (mCoolingSetPointCelsius != coolingSetpoint) {
            mCoolingSetPointCelsius = coolingSetpoint;
            if (!mUpdatingFromPoll) {
                LOG_INF("Cooling TEMP -> %d", coolingSetpoint);
                if (mSystemMode == app::Clusters::Thermostat::SystemModeEnum::kCool ||
                    mSystemMode == app::Clusters::Thermostat::SystemModeEnum::kAuto) {
                    mPendingCommandFlags.fetch_or(kCommandOperation);
                    k_work_submit_to_queue(&mS21WorkQueue, &mCommandWork);
                }
            }
        }
        else {
            LOG_DBG("Cooling TEMP: %d (unchanged)", coolingSetpoint);
        }
    } break;

    case OccupiedHeatingSetpoint::Id: {
        auto heatingSetpoint = *reinterpret_cast<int16_t*>(value);
        if (mHeatingSetPointCelsius != heatingSetpoint) {
            mHeatingSetPointCelsius = heatingSetpoint;
            if (!mUpdatingFromPoll) {
                LOG_INF("Heating TEMP -> %d", heatingSetpoint);
                if (mSystemMode == app::Clusters::Thermostat::SystemModeEnum::kHeat ||
                    mSystemMode == app::Clusters::Thermostat::SystemModeEnum::kAuto) {
                    mPendingCommandFlags.fetch_or(kCommandOperation);
                    k_work_submit_to_queue(&mS21WorkQueue, &mCommandWork);
                }
            }
        }
        else {
            LOG_DBG("Heating TEMP: %d (unchanged)", heatingSetpoint);
        }
    } break;

    case SystemMode::Id: {
        auto systemMode = static_cast<app::Clusters::Thermostat::SystemModeEnum>(*value);
        if (mSystemMode != systemMode) {
            mSystemMode = systemMode;
            if (!mUpdatingFromPoll) {
                LOG_INF("System Mode changed -> %s", GetSystemModeStr(systemMode));
                if (systemMode != app::Clusters::Thermostat::SystemModeEnum::kOff) {
                    mPendingCommandFlags.fetch_or(kCommandOperation);
                    k_work_submit_to_queue(&mS21WorkQueue, &mCommandWork);
                }
            }
        }
        else {
            LOG_DBG("System Mode: %s (unchanged)", GetSystemModeStr(systemMode));
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
        auto mapped = SpeedSettingToS21FanMode(*value);
        if (mapped) {
            auto fanMode = *mapped;
            if (fanMode != mFanMode) {
                mFanMode = fanMode;
                if (!mUpdatingFromPoll) {
                    LOG_DBG("FanMode %u -> %u", static_cast<uint8_t>(mFanMode), static_cast<uint8_t>(fanMode));
                    mPendingCommandFlags.fetch_or(kCommandOperation);
                    k_work_submit_to_queue(&mS21WorkQueue, &mCommandWork);
                }
            }
            else {
                LOG_DBG("FanMode: %u (unchanged)", static_cast<uint8_t>(fanMode));
            }
        }
        break;
    }
    case Attributes::FanMode::Id: {
        auto fanModeEnum = static_cast<FanModeEnum>(*value);
        if (fanModeEnum == FanModeEnum::kAuto || fanModeEnum == FanModeEnum::kSmart) {
            if (mFanMode != ::FanMode::Auto) {
                mFanMode = ::FanMode::Auto;
                if (!mUpdatingFromPoll) {
                    LOG_DBG("FanMode %u -> Auto", static_cast<uint8_t>(mFanMode));
                    mPendingCommandFlags.fetch_or(kCommandOperation);
                    k_work_submit_to_queue(&mS21WorkQueue, &mCommandWork);
                }
            }
            else {
                LOG_DBG("FanMode: Auto (unchanged)");
            }
        }
        else {
            // Non-Auto FanMode changes are always accompanied by a SpeedSetting change,
            // which is the authoritative handler for discrete fan speeds.
            LOG_DBG("FanMode %u (non-Auto): handled via SpeedSetting", static_cast<uint8_t>(fanModeEnum));
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
        auto mode        = SystemModeToOperatingMode(mSystemMode);
        int16_t setpoint;

        switch (mode) {
        case OperatingMode::Auto:
        case OperatingMode::Auto_Cooling:
            setpoint = mCoolingSetPointCelsius - kDeadbandOffsetCelsius;
            break;
        case OperatingMode::Auto_Heating:
            setpoint = mHeatingSetPointCelsius + kDeadbandOffsetCelsius;
            break;
        case OperatingMode::Cool:
            setpoint = mCoolingSetPointCelsius;
            break;
        case OperatingMode::Heat:
            setpoint = mHeatingSetPointCelsius;
            break;
        default:
            break;
        }

        LOG_INF("Sending setOperation(onOff=%s, mode=%u, setpoint=%d, fan=%u) to S21",
                mOnOff ? "true" : "false", static_cast<uint8_t>(mode), setpoint,
                static_cast<uint8_t>(mFanMode));
        auto result = mS21Manager->setOperation(mOnOff, mode, setpoint, mFanMode);
        if (!result) LOG_WRN("setOperation failed: %s", result.error().message);
    }
}

OperatingMode AirConditionerManager::SystemModeToOperatingMode(
    Clusters::Thermostat::SystemModeEnum mode)
{
    using SystemModeEnum = Clusters::Thermostat::SystemModeEnum;
    switch (mode) {
    case SystemModeEnum::kCool:          return OperatingMode::Cool;
    case SystemModeEnum::kHeat:          return OperatingMode::Heat;
    case SystemModeEnum::kDry:           return OperatingMode::Dry;
    case SystemModeEnum::kFanOnly:       return OperatingMode::FanOnly;
    case SystemModeEnum::kEmergencyHeat: return OperatingMode::Heat;
    case SystemModeEnum::kPrecooling:    return OperatingMode::Cool;
    case SystemModeEnum::kAuto:
    default:                             return OperatingMode::Auto;
    }
}

std::optional<::FanMode> AirConditionerManager::SpeedSettingToS21FanMode(uint8_t rawValue)
{
    // SpeedSetting is a nullable uint8_t; the null sentinel is 0xFF
    if (rawValue == 0xFF) return ::FanMode::Auto;  // null → Auto
    if (rawValue == 0)    return std::nullopt;      // 0 (power-off) → no-op
    switch (rawValue) {
    case 1: return ::FanMode::Quiet;
    case 2: return ::FanMode::Low;
    case 3: return ::FanMode::MidLow;
    case 4: return ::FanMode::Medium;
    case 5: return ::FanMode::MidHigh;
    case 6: return ::FanMode::High;
    default:
        LOG_WRN("SpeedSetting value %u out of range [1,6], ignoring", rawValue);
        return std::nullopt;
    }
}

void AirConditionerManager::UpdatePowerIndicator()
{
    gpio_pin_set_dt(&led0, mOnOff);
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
