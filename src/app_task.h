/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include "board/board.h"

#include <platform/CHIPDeviceLayer.h>

struct Identify;

enum class TemperatureButtonAction : uint8_t {
    Pushed,
    Released
};

/**
 * AppTask is the top-level application singleton. Brings up the Matter
 * stack, the S21 transport, the temperature sensor manager, and the
 * AirConditionerManager during StartApp(), and routes board-level button
 * events to the appropriate handlers.
 */
class AppTask {
  public:
    static AppTask& Instance()
    {
        static AppTask sAppTask;
        return sAppTask;
    };

    CHIP_ERROR StartApp();

  private:
    CHIP_ERROR Init();

    static void ButtonEventHandler(Nrf::ButtonState state, Nrf::ButtonMask hasChanged);
    static void ThermostatHandler(const TemperatureButtonAction& action);
};
