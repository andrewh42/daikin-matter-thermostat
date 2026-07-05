/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#pragma once

#include "fan_mapping.h"
#include "logical_ac_state.h"
#include "logical_attribute.h"
#include "operational_mode.h"
#include "twin_field.h"

#include <app-common/zap-generated/attributes/Accessors.h>
// Production needs cluster-objects.h for the cluster::Id and
// attribute::Id constants; host tests get the same constants directly out
// of the chip_stubs Accessors.h above. Guarded so host tests still work.
#if __has_include(<app-common/zap-generated/cluster-objects.h>)
#include <app-common/zap-generated/cluster-objects.h>
#endif

#include <cstdint>
#include <optional>
#include <utility>

/**
 * sync_aai holds the pure-function translation tables between sync/
 * domain types and the CHIP cluster types they map to at the AAI
 * boundary. Lives in inline functions so the AAI Read/Write switches
 * stay one-line-per-attribute.
 *
 * Host-testable: only depends on `<app-common/zap-generated/attributes/Accessors.h>`,
 * which `tests/sync/chip_stubs/` mirrors. The boundary tables are pinned
 * by test_aai_translation.cpp.
 */
namespace sync_aai {

// ─── SystemMode ↔ (power, OperationalMode) ──────────────────────────────────

inline chip::app::Clusters::Thermostat::SystemModeEnum
toMatterSystemMode(bool onOff, sync::OperationalMode m)
{
    using SM = chip::app::Clusters::Thermostat::SystemModeEnum;
    if (!onOff) return SM::kOff;
    switch (m) {
    case sync::OperationalMode::Auto:    return SM::kAuto;
    case sync::OperationalMode::Cool:    return SM::kCool;
    case sync::OperationalMode::Heat:    return SM::kHeat;
    case sync::OperationalMode::Dry:     return SM::kDry;
    case sync::OperationalMode::FanOnly: return SM::kFanOnly;
    }
    return SM::kAuto; // unreachable; silences -Wreturn-type
}

/// Returns nullopt for non-spec values (e.g. kSleep), which the AAI maps
/// to ConstraintError. Reserved/unrecognised values likewise fail closed.
inline std::optional<std::pair<bool, sync::OperationalMode>>
fromMatterSystemMode(chip::app::Clusters::Thermostat::SystemModeEnum v)
{
    using SM = chip::app::Clusters::Thermostat::SystemModeEnum;
    switch (v) {
    case SM::kOff:           return std::pair{false, sync::OperationalMode::Auto};
    case SM::kAuto:          return std::pair{true,  sync::OperationalMode::Auto};
    case SM::kCool:          return std::pair{true,  sync::OperationalMode::Cool};
    case SM::kHeat:          return std::pair{true,  sync::OperationalMode::Heat};
    case SM::kEmergencyHeat: return std::pair{true,  sync::OperationalMode::Heat};
    case SM::kPrecooling:    return std::pair{true,  sync::OperationalMode::Cool};
    case SM::kDry:           return std::pair{true,  sync::OperationalMode::Dry};
    case SM::kFanOnly:       return std::pair{true,  sync::OperationalMode::FanOnly};
    case SM::kSleep:
    default:                 return std::nullopt;
    }
}

// ─── RunningMode ────────────────────────────────────────────────────────────

inline chip::app::Clusters::Thermostat::ThermostatRunningModeEnum
toMatterRunningMode(sync::RunningMode r)
{
    using RM = chip::app::Clusters::Thermostat::ThermostatRunningModeEnum;
    switch (r) {
    case sync::RunningMode::Off:     return RM::kOff;
    case sync::RunningMode::Cooling: return RM::kCool;
    case sync::RunningMode::Heating: return RM::kHeat;
    }
    return RM::kOff; // unreachable; silences -Wreturn-type
}

// ─── SetpointChangeSource ───────────────────────────────────────────────────

inline chip::app::Clusters::Thermostat::SetpointChangeSourceEnum
toMatterSetpointSource(sync::ObservationSource s)
{
    using SS = chip::app::Clusters::Thermostat::SetpointChangeSourceEnum;
    switch (s) {
    case sync::ObservationSource::Matter: return SS::kExternal;
    case sync::ObservationSource::Device: return SS::kManual;
    case sync::ObservationSource::Boot:
    default:                              return SS::kManual;
    }
}

// ─── FanMode ↔ domain mapping ───────────────────────────────────────────────

/// Projects the domain coarse fan mode to the cluster FanModeEnum.
inline chip::app::Clusters::FanControl::FanModeEnum
toMatterFanMode(sync::FanModeCategory c)
{
    using FM = chip::app::Clusters::FanControl::FanModeEnum;
    switch (c) {
    case sync::FanModeCategory::Off:    return FM::kOff;
    case sync::FanModeCategory::Low:    return FM::kLow;
    case sync::FanModeCategory::Medium: return FM::kMedium;
    case sync::FanModeCategory::High:   return FM::kHigh;
    case sync::FanModeCategory::Auto:   return FM::kAuto;
    }
    return FM::kAuto; // unreachable; silences -Wreturn-type
}

/// How the AAI should act on a FanMode write (§4.4.6.1):
///   - PowerOff: Off → turn the AC off (the fan can't stop independently).
///   - SetSpeed: set the fan to `speed` (Auto/Smart → nullopt; Low/Medium/
///     High → representative level; On → High).
///   - Reject:   value not supported by FanModeSequence → CONSTRAINT_ERROR.
enum class FanModeWriteKind { PowerOff, SetSpeed, Reject };
struct FanModeWrite {
    FanModeWriteKind kind;
    sync::FanSpeed   speed; ///< meaningful only when kind == SetSpeed
};

inline FanModeWrite
fromMatterFanMode(chip::app::Clusters::FanControl::FanModeEnum v)
{
    using FM  = chip::app::Clusters::FanControl::FanModeEnum;
    using Cat = sync::FanModeCategory;
    switch (v) {
    case FM::kOff:    return {FanModeWriteKind::PowerOff, {}};
    case FM::kLow:    return {FanModeWriteKind::SetSpeed, sync::FanSpeed{sync::representativeSpeed(Cat::Low)}};
    case FM::kMedium: return {FanModeWriteKind::SetSpeed, sync::FanSpeed{sync::representativeSpeed(Cat::Medium)}};
    case FM::kHigh:   return {FanModeWriteKind::SetSpeed, sync::FanSpeed{sync::representativeSpeed(Cat::High)}};
    case FM::kOn:     return {FanModeWriteKind::SetSpeed, sync::FanSpeed{sync::representativeSpeed(Cat::High)}}; // On → High (§4.4.6.1.3)
    case FM::kAuto:
    case FM::kSmart:  return {FanModeWriteKind::SetSpeed, sync::FanSpeed{std::nullopt}};                       // AUT supported
    }
    return {FanModeWriteKind::Reject, {}};
}

// ─── LogicalAttribute → (ClusterId, AttributeId) ────────────────────────────

/// Maps each LogicalAttribute to the (cluster, attribute) coordinates a
/// Matter subscriber sees. Reachable maps to {0, 0} as a sentinel meaning
/// "no externally visible Matter address" until the BridgedDeviceBasicInfo
/// cluster is added to ZAP (Phase 8); the listener filters those out.
inline std::pair<chip::ClusterId, chip::AttributeId>
toMatterAddress(sync::LogicalAttribute a)
{
    namespace Cl     = chip::app::Clusters;
    namespace TAttr  = Cl::Thermostat::Attributes;
    namespace FCAttr = Cl::FanControl::Attributes;
    namespace OOAttr = Cl::OnOff::Attributes;
    namespace RHAttr = Cl::RelativeHumidityMeasurement::Attributes;
    switch (a) {
    case sync::LogicalAttribute::OnOff:
        return {Cl::OnOff::Id, OOAttr::OnOff::Id};
    case sync::LogicalAttribute::SystemMode:
        return {Cl::Thermostat::Id, TAttr::SystemMode::Id};
    case sync::LogicalAttribute::OccupiedHeatingSetpoint:
        return {Cl::Thermostat::Id, TAttr::OccupiedHeatingSetpoint::Id};
    case sync::LogicalAttribute::OccupiedCoolingSetpoint:
        return {Cl::Thermostat::Id, TAttr::OccupiedCoolingSetpoint::Id};
    case sync::LogicalAttribute::RunningMode:
        return {Cl::Thermostat::Id, TAttr::ThermostatRunningMode::Id};
    case sync::LogicalAttribute::LocalTemperature:
        return {Cl::Thermostat::Id, TAttr::LocalTemperature::Id};
    case sync::LogicalAttribute::OutdoorTemperature:
        return {Cl::Thermostat::Id, TAttr::OutdoorTemperature::Id};
    case sync::LogicalAttribute::SetpointSource:
        return {Cl::Thermostat::Id, TAttr::SetpointChangeSource::Id};
    case sync::LogicalAttribute::SpeedSetting:
        return {Cl::FanControl::Id, FCAttr::SpeedSetting::Id};
    case sync::LogicalAttribute::FanMode:
        return {Cl::FanControl::Id, FCAttr::FanMode::Id};
    case sync::LogicalAttribute::SpeedCurrent:
        return {Cl::FanControl::Id, FCAttr::SpeedCurrent::Id};
    case sync::LogicalAttribute::PercentSetting:
        return {Cl::FanControl::Id, FCAttr::PercentSetting::Id};
    case sync::LogicalAttribute::PercentCurrent:
        return {Cl::FanControl::Id, FCAttr::PercentCurrent::Id};
    case sync::LogicalAttribute::Humidity:
        return {Cl::RelativeHumidityMeasurement::Id, RHAttr::MeasuredValue::Id};
    case sync::LogicalAttribute::Reachable:
        // BridgedDeviceBasicInformation::Reachable — cluster not yet in
        // ZAP. Caller filters this out.
        return {0, 0};
    }
    return {0, 0};
}

// ─── Optional ↔ chip::Nullable wrapping for AAI Encode ──────────────────────

template <typename T>
inline chip::app::DataModel::Nullable<T> wrap(const std::optional<T>& v)
{
    chip::app::DataModel::Nullable<T> out;
    if (v.has_value()) out.SetNonNull(*v);
    else               out.SetNull();
    return out;
}

} // namespace sync_aai
