/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_task.h"

#include "airconditioner_manager.h"
#include "s21/s21_stack.h"
#include "sync/sync_coordinator.h"

#include "app/matter_init.h"
#include "app/task_executor.h"
#include "clusters/identify.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include <openthread/thread.h>
#include <openthread.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app, CONFIG_MATTER_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;

namespace {
constexpr EndpointId kThermostatEndpointId = 1;

Nrf::Matter::IdentifyCluster sIdentifyThermostatCluster(kThermostatEndpointId);

#define TEMPERATURE_BUTTON_MASK DK_BTN2_MSK

constexpr uint32_t kThreadStatusIntervalMs = 30000;
k_timer sThreadStatusTimer;

void ThreadStatusTimerHandler()
{
    otInstance *instance = openthread_get_default_instance();
    if (!instance) {
        return;
    }

    int8_t avgRssi, lastRssi;

    openthread_mutex_lock();
    otError avgErr = otThreadGetParentAverageRssi(instance, &avgRssi);
    otError lastErr = otThreadGetParentLastRssi(instance, &lastRssi);
    openthread_mutex_unlock();

    if (avgErr == OT_ERROR_NONE && lastErr == OT_ERROR_NONE) {
        LOG_INF("Thread parent RSSI: avg=%d dBm, last=%d dBm", avgRssi, lastRssi);
    }
}

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
    int err = S21Stack::Instance().Init();
    if (err) {
        LOG_ERR("S21Stack initialization failed: %d", err);
        return chip::System::MapErrorZephyr(err);
    }

    /* Initialize Matter stack */
    ReturnErrorOnFailure(Nrf::Matter::PrepareServer(Nrf::Matter::InitData{.mPostServerInitClbk = [] {
        // SyncCoordinator must Init *after* the Matter Server has run plugin
        // init callbacks (so the SDK's gThermostatAttrAccess is registered
        // and we can unregister-then-replace it cleanly).
        CHIP_ERROR err = sync::SyncCoordinator::Instance().Init(/*endpoint=*/1);
        if (err != CHIP_NO_ERROR) {
            LOG_ERR("SyncCoordinator Init fail: 0x%" PRIx32, err.AsInteger());
            return err;
        }
        err = AirConditionerManager::Instance().Init(S21Stack::Instance().GetManager(),
                                                     sync::SyncCoordinator::Instance());
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

    ReturnErrorOnFailure(sIdentifyThermostatCluster.Init());

    /* Start periodic Thread RSSI logging */
    k_timer_init(&sThreadStatusTimer,
        [](k_timer *) { Nrf::PostTask([] { ThreadStatusTimerHandler(); }); },
        nullptr);
    k_timer_start(&sThreadStatusTimer, K_MSEC(kThreadStatusIntervalMs), K_MSEC(kThreadStatusIntervalMs));

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
