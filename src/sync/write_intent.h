/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * WriteIntent
 * -----------
 * Closed sum-type of cluster-attribute writes the bridge accepts from
 * Matter controllers. Mirrors the externalised attributes 1-to-1; mode-
 * aware routing (e.g. "what does writing OccupiedHeatingSetpoint mean in
 * Auto mode?") is the reconciler's job, not the intent's.
 *
 * SetSystemModeIntent carries power and operating mode together because
 * the Matter SystemMode write inherently does (kOff means power off;
 * kCool/kHeat/etc. mean power on and select that mode). The AAI Write
 * decodes once and emits one intent; the reconciler applies both axes
 * atomically. `mode` is meaningful only when `power=true`.
 */
#pragma once

#include "logical_ac_state.h"

#include <cstdint>
#include <variant>

namespace sync {

struct SetOnOffIntent                   { bool value; };
struct SetSystemModeIntent              { bool power; OperationalMode mode; };
struct SetOccupiedHeatingSetpointIntent { int16_t value; };
struct SetOccupiedCoolingSetpointIntent { int16_t value; };
struct SetSpeedSettingIntent            { FanSpeed value; };

using WriteIntent = std::variant<
    SetOnOffIntent,
    SetSystemModeIntent,
    SetOccupiedHeatingSetpointIntent,
    SetOccupiedCoolingSetpointIntent,
    SetSpeedSettingIntent
>;

} // namespace sync
