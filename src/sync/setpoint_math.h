/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#pragma once

#include <cstdint>

/**
 * sync_aai::applySetpointDelta is the pure arithmetic behind the Thermostat
 * `SetpointRaiseLower` command: it adds the signed command `amount` to a
 * current setpoint and clamps the result to a [lo, hi] limit window.
 *
 * Factored out (CHIP-free, only `<cstdint>`) so the math is host-testable in
 * isolation — the `CommandHandlerInterface` glue in command_router.{h,cpp}
 * that decodes the command and reads the live setpoints is not.
 */
namespace sync_aai {

/// Apply a `SetpointRaiseLower` delta to a setpoint.
///
/// @param current  The current setpoint, in 0.01 °C units (the unit used
///                 throughout the bridge and the Matter Thermostat cluster).
/// @param amount   The command's signed `amount` field, in 0.1 °C steps
///                 (per the Matter spec); scaled by 10 to 0.01 °C here.
/// @param lo, hi   Inclusive clamp limits, in 0.01 °C units.
/// @return         `clamp(current + amount*10, lo, hi)`. Computed in 32-bit
///                 to avoid int16 overflow before clamping.
inline int16_t applySetpointDelta(int16_t current, int8_t amount, int16_t lo, int16_t hi)
{
    int32_t desired = static_cast<int32_t>(current) + static_cast<int32_t>(amount) * 10;
    if (desired < lo) desired = lo;
    if (desired > hi) desired = hi;
    return static_cast<int16_t>(desired);
}

} // namespace sync_aai
