/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#include "projector.h"

namespace sync {

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

OperationalMode Projector::projectedMode(const LogicalACState& s) const
{
    return projectedValue(s.mode);
}

int16_t Projector::projectedOccupiedHeatingSetpoint(const LogicalACState& s) const
{
    // The band edge follows the projected mode; if the controller flipped
    // mode to Auto and that write is queued, controllers should already
    // see auto-band semantics on read.
    if (projectedValue(s.mode) == OperationalMode::Auto) {
        return static_cast<int16_t>(projectedValue(s.autoSetpoint)
                                    - mConfig.autoBandHalfWidthCentiC);
    }
    return projectedValue(s.heatSetpoint);
}

int16_t Projector::projectedOccupiedCoolingSetpoint(const LogicalACState& s) const
{
    if (projectedValue(s.mode) == OperationalMode::Auto) {
        return static_cast<int16_t>(projectedValue(s.autoSetpoint)
                                    + mConfig.autoBandHalfWidthCentiC);
    }
    return projectedValue(s.coolSetpoint);
}

RunningMode Projector::projectedRunningMode(const LogicalACState& s) const
{
    // Power off → no compressor activity, regardless of fused direction.
    if (!projectedValue(s.onOff)) return RunningMode::Off;
    return s.runningMode.observed();
}

std::optional<int16_t> Projector::projectedLocalTemperature(const LogicalACState& s) const
{
    return s.indoorTemp.observed();
}

std::optional<int16_t> Projector::projectedOutdoorTemperature(const LogicalACState& s) const
{
    return s.outdoorTemp.observed();
}

ObservationSource Projector::projectedSetpointSource(const LogicalACState& s) const
{
    // attribution() (not lastObservedSource()) is what we want here: a
    // Matter intent confirmed by a Device-sourced poll still attributes
    // to External, because the controller is the rightful owner of the
    // value even though the confirming bytes came in via the device.
    return s.activeSetpoint(s.mode.observed()).attribution();
}

std::optional<uint8_t> Projector::projectedSpeedSetting(const LogicalACState& s) const
{
    // Power-coupled: the fan is off when the unit is off → SpeedSetting 0
    // (the spec's 0/Off range). Otherwise null ⇔ Auto, else the level value.
    if (!projectedOnOff(s)) return uint8_t{0};
    const FanSpeed fan = projectedValue(s.fan);
    if (!fan.has_value()) return std::nullopt;
    return static_cast<uint8_t>(*fan);
}

FanModeCategory Projector::projectedFanMode(const LogicalACState& s) const
{
    // Off comes from the power axis; the running ranges from the level.
    if (!projectedOnOff(s)) return FanModeCategory::Off;
    return fanModeOf(projectedValue(s.fan));
}

uint8_t Projector::projectedSpeedCurrent(const LogicalACState& s) const
{
    // "zero to indicate that the fan is off" (§4.4.6.7) when powered off.
    if (!projectedOnOff(s)) return 0;
    // Mid-range tachometer indication while in Auto, so legacy controllers
    // showing a 0..N bar don't render an empty bar.
    return static_cast<uint8_t>(projectedValue(s.fan).value_or(FanLevel::MidLow));
}

std::optional<uint8_t> Projector::projectedPercentSetting(const LogicalACState& s) const
{
    // Mirrors SpeedSetting's three states: 0 when off, null ⇔ Auto, else the
    // remembered exact controller percent (never the lossy floor of the level).
    if (!projectedOnOff(s)) return uint8_t{0};
    return s.fanPercentSetting;
}

uint8_t Projector::projectedPercentCurrent(const LogicalACState& s) const
{
    // Pure function of the current speed (0 when off), per §4.4.6.4.
    return speedLevelToPercent(static_cast<FanLevel>(projectedSpeedCurrent(s)));
}

std::optional<uint16_t> Projector::projectedHumidityCentiPercent(const LogicalACState& s) const
{
    const auto& v = s.humidity.observed();
    if (!v.has_value()) return std::nullopt;
    return static_cast<uint16_t>(*v) * 100;
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
        .mode                    = projectedMode(s),
        .occupiedHeatingSetpoint = projectedOccupiedHeatingSetpoint(s),
        .occupiedCoolingSetpoint = projectedOccupiedCoolingSetpoint(s),
        .runningMode             = projectedRunningMode(s),
        .localTemperature        = projectedLocalTemperature(s),
        .outdoorTemperature      = projectedOutdoorTemperature(s),
        .setpointSource          = projectedSetpointSource(s),
        .speedSetting            = projectedSpeedSetting(s),
        .fanMode                 = projectedFanMode(s),
        .speedCurrent            = projectedSpeedCurrent(s),
        .percentSetting          = projectedPercentSetting(s),
        .percentCurrent          = projectedPercentCurrent(s),
        .humidityCentiPercent    = projectedHumidityCentiPercent(s),
        .reachable               = projectedReachable(s),
    };
}

// ─── Diff ────────────────────────────────────────────────────────────────────

std::vector<LogicalAttribute>
diffProjections(const ProjectedClusterState& before,
                const ProjectedClusterState& after)
{
    std::vector<LogicalAttribute> out;
    out.reserve(8);

    // SystemMode is derived from (onOff, mode); emit it if either differs,
    // even though the kernel doesn't store it as a single field.
    bool systemModeEmitted = false;
    auto emitSystemMode = [&] {
        if (!systemModeEmitted) {
            out.push_back(LogicalAttribute::SystemMode);
            systemModeEmitted = true;
        }
    };

    if (before.onOff != after.onOff) {
        out.push_back(LogicalAttribute::OnOff);
        emitSystemMode();
    }
    if (before.mode != after.mode) {
        emitSystemMode();
    }
    if (before.occupiedHeatingSetpoint != after.occupiedHeatingSetpoint)
        out.push_back(LogicalAttribute::OccupiedHeatingSetpoint);

    if (before.occupiedCoolingSetpoint != after.occupiedCoolingSetpoint)
        out.push_back(LogicalAttribute::OccupiedCoolingSetpoint);

    if (before.runningMode != after.runningMode)
        out.push_back(LogicalAttribute::RunningMode);

    if (before.localTemperature != after.localTemperature)
        out.push_back(LogicalAttribute::LocalTemperature);

    if (before.outdoorTemperature != after.outdoorTemperature)
        out.push_back(LogicalAttribute::OutdoorTemperature);

    if (before.setpointSource != after.setpointSource)
        out.push_back(LogicalAttribute::SetpointSource);

    if (before.speedSetting != after.speedSetting)
        out.push_back(LogicalAttribute::SpeedSetting);

    if (before.fanMode != after.fanMode)
        out.push_back(LogicalAttribute::FanMode);

    if (before.speedCurrent != after.speedCurrent)
        out.push_back(LogicalAttribute::SpeedCurrent);

    if (before.percentSetting != after.percentSetting)
        out.push_back(LogicalAttribute::PercentSetting);

    if (before.percentCurrent != after.percentCurrent)
        out.push_back(LogicalAttribute::PercentCurrent);

    if (before.humidityCentiPercent != after.humidityCentiPercent)
        out.push_back(LogicalAttribute::Humidity);

    // Reachable is currently a no-op until BridgedDeviceBasicInformation
    // is added to ZAP (Phase 8). Track the diff for parity with the rest
    // of the projection; listeners filter out Reachable via the {0,0}
    // sentinel in toMatterAddress until Phase 8 lands.
    if (before.reachable != after.reachable)
        out.push_back(LogicalAttribute::Reachable);

    return out;
}

} // namespace sync
