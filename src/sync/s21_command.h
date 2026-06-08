/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Device-side command type that flows from the bridge's reconciler out to
 * the S21 layer. Sibling of s21_observation.h on the inbound side: this
 * file carries the outbound shape, that one carries the inbound shapes.
 *
 * Defined here in sync/ (next to its primary producer, Reconciler::
 * pendingCommand) rather than under s21/ so callers don't pay an s21/
 * header dependency for a type they only ever construct and compare.
 */
#pragma once

#include "../s21/s21_presentation.h" // OperatingMode, FanMode

#include <cstdint>

/// Equality-comparable so reconciler dedup against the last-sent command
/// is a single `==` check.
struct S21OperationCommand {
    bool          onOff           = false;
    OperatingMode operatingMode   = OperatingMode::Auto;
    int16_t       setpointCelsius = 2600; ///< 0.01 °C, single S21 setpoint
    FanMode       fanMode         = FanMode::Auto;

    bool operator==(const S21OperationCommand& o) const
    {
        return onOff           == o.onOff           &&
               operatingMode   == o.operatingMode   &&
               setpointCelsius == o.setpointCelsius &&
               fanMode         == o.fanMode;
    }
};
