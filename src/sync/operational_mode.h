/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#pragma once

#include <cstdint>

namespace sync {

/**
 * OperationalMode names the modes the device can be *driven to*. Power
 * is a separate axis (LogicalACState::onOff), matching how the S21
 * protocol issues independent on/off and operating-mode commands; the
 * cluster-side SystemModeEnum's `kOff` value collapses to `onOff=false`
 * at the AAI boundary.
 */
enum class OperationalMode : uint8_t {
    Auto,
    Cool,
    Heat,
    Dry,
    FanOnly,
};

/**
 * RunningMode names what the device is *currently doing*. Fused at
 * observation time from S21's Auto_Cooling/Auto_Heating direction signal,
 * refrigerantValveOpen (when reported), and indoor/setpoint hysteresis as
 * a temperature-only fallback. Off when the unit reports !onOff.
 */
enum class RunningMode : uint8_t {
    Off,
    Cooling,
    Heating,
};

} // namespace sync
