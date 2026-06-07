/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#include "projector.h"

// The Id constants for cluster attributes live in per-cluster headers
// pulled in via cluster-objects.h. Host tests use a stub Accessors.h that
// declares them inline, so they don't need this include — that's why it
// lives in the .cpp rather than projector.h.
#if __has_include(<app-common/zap-generated/cluster-objects.h>)
#include <app-common/zap-generated/cluster-objects.h>
#endif

namespace sync {

namespace TAttrNs  = chip::app::Clusters::Thermostat::Attributes;
namespace FCAttrNs = chip::app::Clusters::FanControl::Attributes;
namespace OOAttrNs = chip::app::Clusters::OnOff::Attributes;

namespace {

// Per the plan's responsive-UI policy: while a write is in flight, project
// the *observed* value (don't promise something the device hasn't yet
// acknowledged); when there's no in-flight, project the *desired* (so a
// controller sees its write reflected immediately, before the round-trip
// to the device). Once observation confirms in-flight, both branches
// agree and the projection settles.
template <typename T>
const T& projectedValue(const TwinField<T>& t)
{
    return t.inFlight().has_value() ? t.observed() : t.desired();
}

} // namespace

// ─── Per-attribute projections ───────────────────────────────────────────────

bool Projector::projectedOnOff(const LogicalACState& s) const
{
    return projectedValue(s.onOff);
}

SystemModeEnum Projector::projectedSystemMode(const LogicalACState& s) const
{
    return projectedValue(s.mode);
}

int16_t Projector::projectedOccupiedHeatingSetpoint(const LogicalACState& s) const
{
    // The band edge follows the projected mode; if the controller flipped
    // mode to Auto and that write is queued, controllers should already
    // see auto-band semantics on read.
    if (projectedValue(s.mode) == SystemModeEnum::kAuto) {
        return static_cast<int16_t>(projectedValue(s.autoSetpoint)
                                    - mConfig.autoBandHalfWidthCentiC);
    }
    return projectedValue(s.heatSetpoint);
}

int16_t Projector::projectedOccupiedCoolingSetpoint(const LogicalACState& s) const
{
    if (projectedValue(s.mode) == SystemModeEnum::kAuto) {
        return static_cast<int16_t>(projectedValue(s.autoSetpoint)
                                    + mConfig.autoBandHalfWidthCentiC);
    }
    return projectedValue(s.coolSetpoint);
}

ThermostatRunningModeEnum Projector::projectedRunningMode(const LogicalACState& s) const
{
    const int16_t indoor = s.indoorTemp.observed();
    const int16_t hyst   = mConfig.runningModeHysteresisCentiC;

    switch (s.mode.observed()) {
    case SystemModeEnum::kOff:
        return ThermostatRunningModeEnum::kOff;
    case SystemModeEnum::kCool:
    case SystemModeEnum::kPrecooling: {
        const int16_t sp = s.coolSetpoint.observed();
        return (sp + hyst >= indoor) ? ThermostatRunningModeEnum::kOff
                                     : ThermostatRunningModeEnum::kCool;
    }
    case SystemModeEnum::kHeat:
    case SystemModeEnum::kEmergencyHeat: {
        const int16_t sp = s.heatSetpoint.observed();
        return (sp - hyst <= indoor) ? ThermostatRunningModeEnum::kOff
                                     : ThermostatRunningModeEnum::kHeat;
    }
    case SystemModeEnum::kAuto: {
        const int16_t sp = s.autoSetpoint.observed();
        if (indoor > sp + hyst) return ThermostatRunningModeEnum::kCool;
        if (indoor < sp - hyst) return ThermostatRunningModeEnum::kHeat;
        return ThermostatRunningModeEnum::kOff;
    }
    default:
        return ThermostatRunningModeEnum::kOff;
    }
}

std::optional<int16_t> Projector::projectedLocalTemperature(const LogicalACState& s) const
{
    // The S21 protocol returns 0 when no sensor reading is available; map
    // that to nullopt so Matter sees a Null attribute rather than 0.00 °C.
    const auto v = s.indoorTemp.observed();
    return v == 0 ? std::optional<int16_t>{} : std::optional<int16_t>{v};
}

std::optional<int16_t> Projector::projectedOutdoorTemperature(const LogicalACState& s) const
{
    const auto v = s.outdoorTemp.observed();
    return v == 0 ? std::optional<int16_t>{} : std::optional<int16_t>{v};
}

SetpointChangeSourceEnum Projector::projectedSetpointSource(const LogicalACState& s) const
{
    // attribution() (not lastObservedSource()) is what we want here: a
    // Matter intent confirmed by a Device-sourced poll still attributes
    // to External, because the controller is the rightful owner of the
    // value even though the confirming bytes came in via the device.
    const auto& active = s.activeSetpoint(s.mode.observed());
    switch (active.attribution()) {
    case ObservationSource::Matter: return SetpointChangeSourceEnum::kExternal;
    case ObservationSource::Device: return SetpointChangeSourceEnum::kManual;
    case ObservationSource::Boot:
    default:                        return SetpointChangeSourceEnum::kManual;
    }
}

FanSpeed Projector::projectedSpeedSetting(const LogicalACState& s) const
{
    // The cluster reports null while in Auto. We model "Auto" as
    // nullopt at the twin level, so this is a direct passthrough.
    return projectedValue(s.fan);
}

FanModeEnum Projector::projectedFanMode(const LogicalACState& s) const
{
    return projectedValue(s.fan).has_value() ? FanModeEnum::kOn : FanModeEnum::kAuto;
}

uint8_t Projector::projectedSpeedCurrent(const LogicalACState& s) const
{
    // Mid-range tachometer indication while in Auto, so legacy controllers
    // showing a 0..N bar don't render an empty bar.
    return static_cast<uint8_t>(projectedValue(s.fan).value_or(FanLevel::MidLow));
}

uint16_t Projector::projectedHumidityCentiPercent(const LogicalACState& s) const
{
    return static_cast<uint16_t>(s.humidity.observed()) * 100;
}

bool Projector::projectedReachable(const LogicalACState& s) const
{
    return s.reachable.observed();
}

// ─── Composite projection ────────────────────────────────────────────────────

ProjectedClusterState Projector::project(const LogicalACState& s) const
{
    return ProjectedClusterState{
        .onOff                   = projectedOnOff(s),
        .systemMode              = projectedSystemMode(s),
        .occupiedHeatingSetpoint = projectedOccupiedHeatingSetpoint(s),
        .occupiedCoolingSetpoint = projectedOccupiedCoolingSetpoint(s),
        .runningMode             = projectedRunningMode(s),
        .localTemperature        = projectedLocalTemperature(s),
        .outdoorTemperature      = projectedOutdoorTemperature(s),
        .setpointSource          = projectedSetpointSource(s),
        .speedSetting            = projectedSpeedSetting(s),
        .fanMode                 = projectedFanMode(s),
        .speedCurrent            = projectedSpeedCurrent(s),
        .humidityCentiPercent    = projectedHumidityCentiPercent(s),
        .reachable               = projectedReachable(s),
    };
}

// ─── Diff ────────────────────────────────────────────────────────────────────

std::vector<MatterAttributePath>
diffProjections(const ProjectedClusterState& before,
                const ProjectedClusterState& after,
                chip::EndpointId             endpoint)
{
    std::vector<MatterAttributePath> out;
    out.reserve(8);

    auto emit = [&](chip::ClusterId c, chip::AttributeId a) {
        out.push_back({endpoint, c, a});
    };

    if (before.onOff                   != after.onOff)
        emit(chip::app::Clusters::OnOff::Id, OOAttrNs::OnOff::Id);

    if (before.systemMode              != after.systemMode)
        emit(chip::app::Clusters::Thermostat::Id, TAttrNs::SystemMode::Id);

    if (before.occupiedHeatingSetpoint != after.occupiedHeatingSetpoint)
        emit(chip::app::Clusters::Thermostat::Id, TAttrNs::OccupiedHeatingSetpoint::Id);

    if (before.occupiedCoolingSetpoint != after.occupiedCoolingSetpoint)
        emit(chip::app::Clusters::Thermostat::Id, TAttrNs::OccupiedCoolingSetpoint::Id);

    if (before.runningMode             != after.runningMode)
        emit(chip::app::Clusters::Thermostat::Id, TAttrNs::ThermostatRunningMode::Id);

    if (before.localTemperature        != after.localTemperature)
        emit(chip::app::Clusters::Thermostat::Id, TAttrNs::LocalTemperature::Id);

    if (before.outdoorTemperature      != after.outdoorTemperature)
        emit(chip::app::Clusters::Thermostat::Id, TAttrNs::OutdoorTemperature::Id);

    if (before.setpointSource          != after.setpointSource)
        emit(chip::app::Clusters::Thermostat::Id, TAttrNs::SetpointChangeSource::Id);

    if (before.speedSetting            != after.speedSetting)
        emit(chip::app::Clusters::FanControl::Id, FCAttrNs::SpeedSetting::Id);

    if (before.fanMode                 != after.fanMode)
        emit(chip::app::Clusters::FanControl::Id, FCAttrNs::FanMode::Id);

    if (before.speedCurrent            != after.speedCurrent)
        emit(chip::app::Clusters::FanControl::Id, FCAttrNs::SpeedCurrent::Id);

    // Reachable lives in BridgedDeviceBasicInformation; the cluster is
    // added to ZAP in Phase 8. Until then the generated headers don't
    // expose its attribute Ids and the path is unreachable. Once Phase 8
    // lands, uncomment:
    //
    //   if (before.reachable != after.reachable)
    //       emit(chip::app::Clusters::BridgedDeviceBasicInformation::Id,
    //            chip::app::Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id);
    (void)before.reachable;
    (void)after.reachable;

    return out;
}

} // namespace sync
