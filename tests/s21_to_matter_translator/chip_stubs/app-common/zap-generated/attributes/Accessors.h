/*
 * Minimal stub for chip::app::Clusters::Thermostat enums needed by S21ToMatterTranslator.
 * Used only in host unit tests; production builds use the real ZAP-generated headers.
 *
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#pragma once

#include <stdint.h>

namespace chip {
namespace app {
namespace Clusters {
namespace Thermostat {

enum class SystemModeEnum : uint8_t {
    kOff           = 0,
    kAuto          = 1,
    kCool          = 3,
    kHeat          = 4,
    kEmergencyHeat = 5,
    kPrecooling    = 6,
    kFanOnly       = 7,
    kDry           = 8,
    kSleep         = 9,
};

enum class ThermostatRunningModeEnum : uint8_t {
    kOff  = 0,
    kCool = 3,
    kHeat = 4,
};

} // namespace Thermostat
} // namespace Clusters
} // namespace app
} // namespace chip
