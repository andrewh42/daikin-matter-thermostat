/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#pragma once

#include <cstdint>

namespace sync {

/**
 * LogicalAttribute names *what changed* in the bridge's view of the air
 * conditioner, without committing to any particular wire-level coordinate
 * system (Matter cluster/attribute, S21 frame, etc.). Listeners receive a
 * vector of these and translate to whichever address space they need.
 *
 * Membership covers every twin/sensor field that the projector can mark
 * dirty. Adding a new projected field means adding an enumerator here;
 * the boundary translation in aai_translation.h gets a -Wswitch warning
 * if a value goes untranslated.
 */
enum class LogicalAttribute : uint8_t {
    OnOff,
    SystemMode,
    OccupiedHeatingSetpoint,
    OccupiedCoolingSetpoint,
    RunningMode,
    LocalTemperature,
    OutdoorTemperature,
    SetpointSource,
    SpeedSetting,
    FanMode,
    SpeedCurrent,
    Humidity,
    Reachable,
};

} // namespace sync
