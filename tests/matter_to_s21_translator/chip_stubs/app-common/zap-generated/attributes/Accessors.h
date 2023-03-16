/*
 * Minimal stub for chip::app::Clusters::Thermostat enums needed by MatterToS21Translator.
 * Used only in host unit tests; production builds use the real ZAP-generated headers.
 *
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#pragma once

#include <stdint.h>

namespace chip {
namespace app {
namespace DataModel {

template <typename T>
class Nullable {
public:
    Nullable() : mIsNull(true), mValue{} {}
    explicit Nullable(T v) : mIsNull(false), mValue(v) {}
    bool IsNull() const { return mIsNull; }
    T    Value()  const { return mValue; }
    static Nullable Null()    { return Nullable(); }
    static Nullable NonNull(T v) { return Nullable(v); }
private:
    bool mIsNull;
    T    mValue;
};

} // namespace DataModel
} // namespace app
} // namespace chip

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
