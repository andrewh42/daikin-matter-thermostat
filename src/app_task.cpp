/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_task.h"

#include "temp_sensor_manager.h"
#include "airconditioner_manager.h"
#include "s21/S21DataLinkUart.h"
#include "s21/S21Presentation.h"
#include "s21/s21_pinconfig.h"

#include "app/matter_init.h"
#include "app/task_executor.h"
#include "clusters/identify.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app, CONFIG_MATTER_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;

namespace {
constexpr EndpointId kThermostatEndpointId = 1;

Nrf::Matter::IdentifyCluster sIdentifyCluster(kThermostatEndpointId);

#define TEMPERATURE_BUTTON_MASK DK_BTN2_MSK

} /* namespace */

void AppTask::ButtonEventHandler(Nrf::ButtonState state, Nrf::ButtonMask hasChanged)
{
    if (TEMPERATURE_BUTTON_MASK & hasChanged) {
        TemperatureButtonAction action =
                (TEMPERATURE_BUTTON_MASK & state) ? TemperatureButtonAction::Pushed : TemperatureButtonAction::Released;
        Nrf::PostTask([action] { ThermostatHandler(action); });
    }
}

void AppTask::ThermostatHandler(const TemperatureButtonAction& action)
{
    if (action == TemperatureButtonAction::Pushed) {
        AirConditionerManager::Instance().LogThermostatStatus();
    }
}

CHIP_ERROR AppTask::Init()
{
    static S21DataLinkUart dataLink(NRF_UARTE21);
    int err = dataLink.init(s21_pinconfig::kTxPin, s21_pinconfig::kRxPin);
    if (err) {
        LOG_ERR("S21DataLinkUart init failed: %d", err);
        return chip::System::MapErrorZephyr(err);
    }

    static S21Presentation s21Presentation(dataLink);

    /* Initialize Matter stack */
    ReturnErrorOnFailure(Nrf::Matter::PrepareServer(Nrf::Matter::InitData{.mPostServerInitClbk = [] {
        CHIP_ERROR err = TempSensorManager::Instance().Init();
        if (err != CHIP_NO_ERROR) {
            LOG_ERR("TempSensorManager Init fail");
            return err;
        }
        err = AirConditionerManager::Instance().Init(s21Presentation);
        if (err != CHIP_NO_ERROR) {
            LOG_ERR("AirConditionerManager Init fail");
        }
        return err;
    }}));

    if (!Nrf::GetBoard().Init(ButtonEventHandler)) {
        LOG_ERR("User interface initialization failed.");
        return CHIP_ERROR_INCORRECT_STATE;
    }

    /* Register Matter event handler that controls the connectivity status LED based on the captured Matter network
     * state. */
    ReturnErrorOnFailure(Nrf::Matter::RegisterEventHandler(Nrf::Board::DefaultMatterEventHandler, 0));

    ReturnErrorOnFailure(sIdentifyCluster.Init());

    return Nrf::Matter::StartServer();
}

CHIP_ERROR AppTask::StartApp()
{
    ReturnErrorOnFailure(Init());

    while (true) {
        Nrf::DispatchNextTask();
    }

    return CHIP_NO_ERROR;
}
