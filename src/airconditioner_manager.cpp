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

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace chip;
using namespace ::chip::DeviceLayer;
using namespace ::chip::app::Clusters;
using namespace chip::app::Clusters::FanControl::Attributes;
using namespace chip::app::Clusters::RelativeHumidityMeasurement::Attributes;
using namespace chip::app::Clusters::Thermostat::Attributes;
using namespace Protocols::InteractionModel;

constexpr EndpointId kThermostatEndpoint     = 1;
constexpr EndpointId kHumiditySensorEndpoint = 2;

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

} /* namespace */

CHIP_ERROR AirConditionerManager::Init(S21Manager& s21Manager)
{
    mS21Manager = &s21Manager;

    ReturnErrorOnFailure(InitLed());

    CHIP_ERROR err = CHIP_NO_ERROR;

    PlatformMgr().LockChipStack();

    VerifyOrExit(Status::Success == LocalTemperature::Get(kThermostatEndpoint, mLocalTempCelsius),
                 err = CHIP_IM_GLOBAL_STATUS(Failure));
    VerifyOrExit(Status::Success == OutdoorTemperature::Get(kThermostatEndpoint, mOutdoorTempCelsius),
                 err = CHIP_IM_GLOBAL_STATUS(Failure));
    VerifyOrExit(Status::Success == OccupiedCoolingSetpoint::Get(kThermostatEndpoint, &mCoolingCelsiusSetPoint),
                 err = CHIP_IM_GLOBAL_STATUS(Failure));
    VerifyOrExit(Status::Success == OccupiedHeatingSetpoint::Get(kThermostatEndpoint, &mHeatingCelsiusSetPoint),
                 err = CHIP_IM_GLOBAL_STATUS(Failure));
    VerifyOrExit(Status::Success == SystemMode::Get(kThermostatEndpoint, &mThermMode),
                 err = CHIP_IM_GLOBAL_STATUS(Failure));

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
        k_work_reschedule_for_queue(&mS21WorkQueue, &mPollWork, K_SECONDS(kS21PollIntervalSec));
    }
    else {
        LOG_DBG("S21Manager initialisation failed, will retry initialisation in %d ms", mInitRetryIntervalMs);
        k_work_reschedule_for_queue(&mS21WorkQueue, &mInitRetryWork, K_MSEC(mInitRetryIntervalMs));
    }

    LOG_DBG("AirConditionerManager initialised successfully");
    return CHIP_NO_ERROR;
}

void AirConditionerManager::PollWorkHandler(k_work* work)
{
    auto* dwork = k_work_delayable_from_work(work);
    auto& self  = *CONTAINER_OF(dwork, AirConditionerManager, mPollWork);

    self.PollSensors();
    self.PollOperation();

    k_work_reschedule_for_queue(&self.mS21WorkQueue, &self.mPollWork, K_SECONDS(kS21PollIntervalSec));
}

void AirConditionerManager::PollSensors()
{
    auto indoor   = mS21Manager->getRoomTemperature();
    auto outdoor  = mS21Manager->getOutdoorTemperature();
    auto humidity = mS21Manager->getHumidity();

    if (!indoor)   LOG_WRN("getRoomTemperature failed: %s",    indoor.error().message);
    if (!outdoor)  LOG_INF("getOutdoorTemperature failed: %s", outdoor.error().message);
    if (!humidity) LOG_INF("getHumidity failed: %s",           humidity.error().message);

    bool indoorChanged   = indoor   && (mLocalTempCelsius.IsNull()  || mLocalTempCelsius.Value()  != *indoor);
    bool outdoorChanged  = outdoor  && (mOutdoorTempCelsius.IsNull() || mOutdoorTempCelsius.Value() != *outdoor);
    bool humidityChanged = humidity && (mHumidity.IsNull()           || mHumidity.Value()           != *humidity * 100);

    if (indoorChanged || outdoorChanged || humidityChanged) {
        if (indoorChanged)   mLocalTempCelsius.SetNonNull(*indoor);
        if (outdoorChanged)  mOutdoorTempCelsius.SetNonNull(*outdoor);
        if (humidityChanged) mHumidity.SetNonNull(*humidity * 100);

        std::optional<int16_t>  indoorVal    = indoorChanged   ? std::optional(*indoor)           : std::nullopt;
        std::optional<int16_t>  outdoorVal   = outdoorChanged  ? std::optional(*outdoor)          : std::nullopt;
        std::optional<uint16_t> humidityVal  = humidityChanged ? std::optional<uint16_t>(*humidity * 100) : std::nullopt;
        Nrf::PostTask([indoorVal, outdoorVal, humidityVal] {
            PlatformMgr().LockChipStack();
            if (indoorVal)   LocalTemperature::Set(kThermostatEndpoint, *indoorVal);
            if (outdoorVal)  OutdoorTemperature::Set(kThermostatEndpoint, *outdoorVal);
            if (humidityVal) MeasuredValue::Set(kHumiditySensorEndpoint, *humidityVal);
            PlatformMgr().UnlockChipStack();
        });
    } else {
        LOG_DBG("S21 sensors unchanged, not updating attributes");
    }
}

void AirConditionerManager::PollOperation()
{
    auto op = mS21Manager->getOperation();
    if (!op) {
        LOG_WRN("getOperation failed: %s", op.error().message);
        return;
    }

    auto [onOff, mode, setpoint, fanMode] = *op;
    bool fanModeChanged = (fanMode != mFanMode);
    mFanMode = fanMode;

    auto systemMode = OperatingModeToSystemMode(mode);

    // Suppress Matter writes while an S21 command is pending: local state already reflects
    // the user's intent, and the command will update S21 (and thus the cache) shortly.
    bool pendingOperation = mPendingCommandFlags.load() & kCommandOperation;
    if (pendingOperation) {
        LOG_DBG("S21 operation pending, skipping attribute update");
        return;
    }

    // Update local state if anything is out of sync with the received S21 state, to avoid AirConditionerManager::AttributeChangeHandler()
    // unnecessarily scheduling AirConditionerManager::CommandWorkHandler to update S21 with the already-set values.
    bool onOffChanged = (onOff != mOnOff);
    mOnOff = onOff;

    bool modeChanged = (systemMode != mThermMode);
    mThermMode = systemMode;

    bool coolingChanged = (mode == OperatingMode::Cool || mode == OperatingMode::Auto_Cooling ||
                           mode == OperatingMode::Auto) &&
                          (setpoint != mCoolingCelsiusSetPoint);
    if (coolingChanged) mCoolingCelsiusSetPoint = setpoint;

    bool heatingChanged = (mode == OperatingMode::Heat || mode == OperatingMode::Auto_Heating ||
                           mode == OperatingMode::Auto) &&
                          (setpoint != mHeatingCelsiusSetPoint);
    if (heatingChanged) mHeatingCelsiusSetPoint = setpoint;

    if (!onOffChanged && !modeChanged && !coolingChanged && !heatingChanged && !fanModeChanged) {
        LOG_DBG("S21 operation unchanged, not updating attributes (S21 setpoint %d, cooling setpoint %d, heating setpoint %d)",
            setpoint, mCoolingCelsiusSetPoint, mHeatingCelsiusSetPoint);
        return;
    }

    Nrf::PostTask([onOff, systemMode, onOffChanged, modeChanged,
                   setpoint, coolingChanged, heatingChanged,
                   fanMode, fanModeChanged] {
        using namespace chip::app::Clusters::FanControl;
        PlatformMgr().LockChipStack();
        if (onOffChanged)   OnOff::Attributes::OnOff::Set(kThermostatEndpoint, onOff);
        if (modeChanged)    SystemMode::Set(kThermostatEndpoint, systemMode);
        if (coolingChanged) OccupiedCoolingSetpoint::Set(kThermostatEndpoint, setpoint);
        if (heatingChanged) OccupiedHeatingSetpoint::Set(kThermostatEndpoint, setpoint);
        if (fanModeChanged) {
            auto speedSetting = S21FanModeToSpeedSetting(fanMode);
            if (speedSetting.has_value()) {
                Attributes::SpeedSetting::Set(kThermostatEndpoint, DataModel::MakeNullable(*speedSetting));
            } else {
                // Auto: write FanMode=kAuto and let the cluster server set SpeedSetting to null.
                // Writing null directly to SpeedSetting is rejected (InvalidInState) by the
                // fan-control-server unless the write originates from cluster logic.
                Attributes::FanMode::Set(kThermostatEndpoint, FanModeEnum::kAuto);
            }
        }
        PlatformMgr().UnlockChipStack();
    });
}

Clusters::Thermostat::SystemModeEnum AirConditionerManager::OperatingModeToSystemMode(OperatingMode mode)
{
    using SystemModeEnum = Clusters::Thermostat::SystemModeEnum;
    switch (mode) {
    case OperatingMode::Cool:         return SystemModeEnum::kCool;
    case OperatingMode::Heat:         return SystemModeEnum::kHeat;
    case OperatingMode::Dry:          return SystemModeEnum::kDry;
    case OperatingMode::FanOnly:      return SystemModeEnum::kFanOnly;
    case OperatingMode::Auto:
    case OperatingMode::Auto_Cooling:
    case OperatingMode::Auto_Heating: return SystemModeEnum::kAuto;
    default:                          return SystemModeEnum::kAuto;
    }
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
        k_work_reschedule_for_queue(&self.mS21WorkQueue, &self.mPollWork, K_SECONDS(kS21PollIntervalSec));
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

const char* AirConditionerManager::GetThermModeStr()
{
    switch (mThermMode) {
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

void AirConditionerManager::LogThermostatStatus()
{
    LOG_INF("Thermostat:");
    LOG_INF("	Mode - %s", GetThermModeStr());
    if (!(GetLocalTemp().IsNull())) {
        int16_t tempValue = GetLocalTemp().Value();
        LOG_INF("	LocalTemperature - %d,%d'C", ReturnCompleteValue(tempValue), ReturnRemainderValue(tempValue));
    }
    else {
        LOG_INF("	LocalTemperature - No Value");
    }
    if (!(GetOutdoorTemp().IsNull())) {
        int16_t tempValue = GetOutdoorTemp().Value();
        LOG_INF("	OutdoorTemperature - %d,%d'C", ReturnCompleteValue(tempValue), ReturnRemainderValue(tempValue));
    }
    else {
        LOG_INF("	OutdoorTemperature - No Value");
    }
    LOG_INF("	HeatingSetpoint - %d,%d'C", ReturnCompleteValue(mHeatingCelsiusSetPoint),
            ReturnRemainderValue(mHeatingCelsiusSetPoint));
    LOG_INF("	CoolingSetpoint - %d,%d'C \n", ReturnCompleteValue(mCoolingCelsiusSetPoint),
            ReturnRemainderValue(mCoolingCelsiusSetPoint));
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
    LOG_INF("Cluster OnOff: attribute OnOff set to %s", mOnOff ? "ON" : "OFF");
    UpdatePowerIndicator();
    mPendingCommandFlags.fetch_or(kCommandOperation);
    k_work_submit_to_queue(&mS21WorkQueue, &mCommandWork);
}

void AirConditionerManager::TemperatureAttributeChangeHandler(AttributeId attributeId, uint8_t* value, uint16_t size)
{
    switch (attributeId) {
    case LocalTemperature::Id:
    case OutdoorTemperature::Id:
        break; // updated by PollWorkHandler on the S21 work queue thread

    case OccupiedCoolingSetpoint::Id: {
        auto coolingSetpoint = *reinterpret_cast<int16_t*>(value);
        if (mCoolingCelsiusSetPoint != coolingSetpoint) {
            LOG_INF("Cooling TEMP %d -> %d", mCoolingCelsiusSetPoint, coolingSetpoint);
            mCoolingCelsiusSetPoint = coolingSetpoint;
            if (mThermMode == app::Clusters::Thermostat::SystemModeEnum::kCool ||
                mThermMode == app::Clusters::Thermostat::SystemModeEnum::kAuto) {
                mPendingCommandFlags.fetch_or(kCommandOperation);
                k_work_submit_to_queue(&mS21WorkQueue, &mCommandWork);
            }
        }
        else {
            LOG_DBG("Cooling TEMP: %d (unchanged)", coolingSetpoint);
        }
    } break;

    case OccupiedHeatingSetpoint::Id: {
        auto heatingSetpoint = *reinterpret_cast<int16_t*>(value);
        if (mHeatingCelsiusSetPoint != heatingSetpoint) {
            LOG_INF("Heating TEMP %d -> %d", mHeatingCelsiusSetPoint, heatingSetpoint);
            mHeatingCelsiusSetPoint = heatingSetpoint;
            if (mThermMode == app::Clusters::Thermostat::SystemModeEnum::kHeat ||
                mThermMode == app::Clusters::Thermostat::SystemModeEnum::kAuto) {
                mPendingCommandFlags.fetch_or(kCommandOperation);
                k_work_submit_to_queue(&mS21WorkQueue, &mCommandWork);
            }
        }
        else {
            LOG_DBG("Heating TEMP: %d (unchanged)", heatingSetpoint);
        }
    } break;

    case SystemMode::Id: {
        auto thermMode = static_cast<app::Clusters::Thermostat::SystemModeEnum>(*value);
        if (mThermMode != thermMode) {
            mThermMode = thermMode;
            LOG_INF("System Mode changed to %s", GetThermModeStr());
            if (mThermMode != app::Clusters::Thermostat::SystemModeEnum::kOff) {
                mPendingCommandFlags.fetch_or(kCommandOperation);
                k_work_submit_to_queue(&mS21WorkQueue, &mCommandWork);
            }
        }
        else {
            LOG_DBG("System Mode: %s (unchanged)", GetThermModeStr());
        }
    } break;

    default: {
        LOG_ERR("Unhandled thermostat attribute %x", attributeId);
        return;
    } break;
    }

    LogThermostatStatus();
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
                LOG_DBG("FanMode %u -> %u", static_cast<uint8_t>(mFanMode), static_cast<uint8_t>(fanMode));
                mFanMode = fanMode;
                mPendingCommandFlags.fetch_or(kCommandOperation);
                k_work_submit_to_queue(&mS21WorkQueue, &mCommandWork);
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
                LOG_DBG("FanMode %u -> Auto", static_cast<uint8_t>(mFanMode));
                mFanMode = ::FanMode::Auto;
                mPendingCommandFlags.fetch_or(kCommandOperation);
                k_work_submit_to_queue(&mS21WorkQueue, &mCommandWork);
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
        auto mode        = SystemModeToOperatingMode(mThermMode);
        int16_t setpoint = (mode == OperatingMode::Heat || mode == OperatingMode::Auto_Heating)
                           ? mHeatingCelsiusSetPoint : mCoolingCelsiusSetPoint;
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

std::optional<uint8_t> AirConditionerManager::S21FanModeToSpeedSetting(::FanMode fanMode)
{
    switch (fanMode) {
    case ::FanMode::Auto:    return std::nullopt;
    case ::FanMode::Quiet:   return 1;
    case ::FanMode::Low:     return 2;
    case ::FanMode::MidLow:  return 3;
    case ::FanMode::Medium:  return 4;
    case ::FanMode::MidHigh: return 5;
    case ::FanMode::High:    return 6;
    default:
        LOG_WRN("S21 FanMode %u unrecognised, defaulting to Auto", static_cast<uint8_t>(fanMode));
        return std::nullopt;
    }
}

void AirConditionerManager::UpdatePowerIndicator()
{
    gpio_pin_set_dt(&led0, mOnOff);
}

app::DataModel::Nullable<int16_t> AirConditionerManager::GetLocalTemp()
{
    return mLocalTempCelsius;
}

app::DataModel::Nullable<int16_t> AirConditionerManager::GetOutdoorTemp()
{
    return mOutdoorTempCelsius;
}
