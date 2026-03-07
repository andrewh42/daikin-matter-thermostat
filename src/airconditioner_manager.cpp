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

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace chip;
using namespace ::chip::DeviceLayer;
using namespace ::chip::app::Clusters;
using namespace chip::app::Clusters::FanControl::Attributes;
using namespace chip::app::Clusters::Thermostat::Attributes;
using namespace Protocols::InteractionModel;

constexpr EndpointId kThermostatEndpoint = 1;

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

    auto indoor  = self.mS21Manager->getRoomTemperature();
    auto outdoor = self.mS21Manager->getOutdoorTemperature();

    if (!indoor)  LOG_WRN("getRoomTemperature failed: %s",    indoor.error().message);
    if (!outdoor) LOG_WRN("getOutdoorTemperature failed: %s", outdoor.error().message);

    if (indoor || outdoor) {
        std::optional<int16_t> indoorVal  = indoor  ? std::optional(*indoor)  : std::nullopt;
        std::optional<int16_t> outdoorVal = outdoor ? std::optional(*outdoor) : std::nullopt;
        Nrf::PostTask([indoorVal, outdoorVal] {
            PlatformMgr().LockChipStack();
            if (indoorVal)  LocalTemperature::Set(kThermostatEndpoint, *indoorVal);
            if (outdoorVal) OutdoorTemperature::Set(kThermostatEndpoint, *outdoorVal);
            PlatformMgr().UnlockChipStack();
        });
    }

    k_work_reschedule_for_queue(&self.mS21WorkQueue, &self.mPollWork, K_SECONDS(kS21PollIntervalSec));
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
        LOG_INF("FanControl cluster attribute changed: Attribute ID %u, Value: %u, Size: %u",
                attributePath.mAttributeId, *value, size);
        break;
    case Thermostat::Id:
        TemperatureAttributeChangeHandler(attributePath.mAttributeId, value, size);
        break;
    case OnOff::Id:
        OnOffAttributeChangeHandler(attributePath.mAttributeId, value, size);
        break;
    default:
        LOG_INF("Unhandled cluster ID: %u", attributePath.mClusterId);
    }
}

void AirConditionerManager::OnOffAttributeChangeHandler(AttributeId attributeId, uint8_t* value, uint16_t size)
{
    mOnOff = *value;
    LOG_INF("Cluster OnOff: attribute OnOff set to %s", mOnOff ? "ON" : "OFF");
    UpdatePowerIndicator();
}

void AirConditionerManager::TemperatureAttributeChangeHandler(AttributeId attributeId, uint8_t* value, uint16_t size)
{
    Status status;

    switch (attributeId) {
    case LocalTemperature::Id: {
        status = LocalTemperature::Get(kThermostatEndpoint, mLocalTempCelsius);
        VerifyOrReturn(Status::Success == status, LOG_ERR("Failed to get Thermostat LocalTemperature attribute"));
    } break;

    case OutdoorTemperature::Id: {
        status = OutdoorTemperature::Get(kThermostatEndpoint, mOutdoorTempCelsius);
        VerifyOrReturn(Status::Success == status, LOG_ERR("Failed to get Thermostat OutdoorTemperature attribute"));
    } break;

    case OccupiedCoolingSetpoint::Id: {
        mCoolingCelsiusSetPoint = *reinterpret_cast<int16_t*>(value);
        LOG_INF("Cooling TEMP: %d", mCoolingCelsiusSetPoint);
    } break;

    case OccupiedHeatingSetpoint::Id: {
        mHeatingCelsiusSetPoint = *reinterpret_cast<int16_t*>(value);
        LOG_INF("Heating TEMP %d", mHeatingCelsiusSetPoint);
    } break;

    case SystemMode::Id: {
        mThermMode = static_cast<app::Clusters::Thermostat::SystemModeEnum>(*value);
        LOG_INF("System Mode changed to %s", GetThermModeStr());
    } break;

    default: {
        LOG_ERR("Unhandled thermostat attribute %x", attributeId);
        return;
    } break;
    }

    LogThermostatStatus();
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
