/*
 * Minimal stub for the chip:: namespace types touched by the sync/ layer.
 * Used only in host unit tests; production builds use the real ZAP-generated
 * headers and CHIP SDK.
 *
 * Mirrors the shape of tests/s21_to_matter_translator/chip_stubs and extends
 * it with enums the reconciler / projector / atomic buffer need
 * (FanControl::FanModeEnum, OnOff/Thermostat/FanControl cluster ids,
 * Thermostat attribute ids, SetpointChangeSourceEnum).
 *
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#pragma once

#include <stdint.h>

// ─── chip::app::DataModel::Nullable<T> ────────────────────────────────────────
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
    static Nullable Null()       { return Nullable(); }
    static Nullable NonNull(T v) { return Nullable(v); }

    bool operator==(const Nullable& o) const {
        return mIsNull == o.mIsNull && (mIsNull || mValue == o.mValue);
    }
private:
    bool mIsNull;
    T    mValue;
};

template <typename T>
Nullable<T> MakeNullable(T v) { return Nullable<T>::NonNull(v); }

} // namespace DataModel
} // namespace app
} // namespace chip

// ─── Cluster IDs and attribute IDs ────────────────────────────────────────────
namespace chip {

using ClusterId   = uint32_t;
using AttributeId = uint32_t;
using EndpointId  = uint16_t;

namespace app {
namespace Clusters {

namespace OnOff {
    constexpr ClusterId Id = 0x0006;
    namespace Attributes {
        namespace OnOff { constexpr AttributeId Id = 0x0000; }
    }
}

namespace Thermostat {

constexpr ClusterId Id = 0x0201;

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

// Matter spec SetpointChangeSourceEnum (Thermostat cluster attribute 0x0030).
enum class SetpointChangeSourceEnum : uint8_t {
    kManual    = 0x00,
    kSchedule  = 0x01,
    kExternal  = 0x02,
};

namespace Attributes {
    namespace LocalTemperature              { constexpr AttributeId Id = 0x0000; }
    namespace OutdoorTemperature            { constexpr AttributeId Id = 0x0001; }
    namespace OccupiedCoolingSetpoint       { constexpr AttributeId Id = 0x0011; }
    namespace OccupiedHeatingSetpoint       { constexpr AttributeId Id = 0x0012; }
    namespace SystemMode                    { constexpr AttributeId Id = 0x001C; }
    namespace ThermostatRunningMode         { constexpr AttributeId Id = 0x001E; }
    namespace SetpointChangeSource          { constexpr AttributeId Id = 0x0030; }
    namespace SetpointChangeSourceTimestamp { constexpr AttributeId Id = 0x0032; }
}
} // namespace Thermostat

namespace FanControl {
constexpr ClusterId Id = 0x0202;

enum class FanModeEnum : uint8_t {
    kOff    = 0,
    kLow    = 1,
    kMedium = 2,
    kHigh   = 3,
    kOn     = 4,
    kAuto   = 5,
    kSmart  = 6,
};

namespace Attributes {
    namespace FanMode       { constexpr AttributeId Id = 0x0000; }
    namespace SpeedSetting  { constexpr AttributeId Id = 0x0004; }
    namespace SpeedCurrent  { constexpr AttributeId Id = 0x0005; }
}
} // namespace FanControl

namespace BridgedDeviceBasicInformation {
constexpr ClusterId Id = 0x0039;
namespace Attributes {
    namespace Reachable { constexpr AttributeId Id = 0x0011; }
}
} // namespace BridgedDeviceBasicInformation

} // namespace Clusters
} // namespace app
} // namespace chip
