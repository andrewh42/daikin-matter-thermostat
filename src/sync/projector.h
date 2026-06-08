/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Projector
 * ---------
 * Pure function: LogicalACState → cluster-attribute values that Matter
 * controllers should see.
 *
 * Two consumers:
 *
 *   - The reconciler diffs project(before) vs project(after) to compute
 *     which attributes need MatterReportingAttributeChangeCallback().
 *
 *   - The AAI Read paths (Phase 5) call per-attribute projectors directly
 *     so a controller read returns the bridge's current view.
 *
 * Auto-mode band synthesis:
 *
 *   - In Auto, occupiedHeatingSetpoint = autoSetpoint − δ
 *                occupiedCoolingSetpoint = autoSetpoint + δ
 *
 *   - In Cool/Heat, the band edge for the *other* side reports its own
 *     shadow (so a mode flip back lands somewhere familiar) rather than a
 *     synthesised δ-band.
 *
 * RunningMode derivation:
 *
 *   - We don't have a dedicated S21 probe for compressor activity, so the
 *     bridge approximates from indoorTemp vs active setpoint with a small
 *     hysteresis. See sync-analysis §4.12.
 *
 * Projections read TwinField::observed() (the "faithful UI" policy): a
 * controller's view tracks what the device has actually acknowledged, not
 * what we hope it will become. desired() leaks would confuse subscribers
 * during the inFlight window.
 */
#pragma once

#include "logical_ac_state.h"
#include "matter_attribute_path.h"

#include <app-common/zap-generated/attributes/Accessors.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace sync {

using ThermostatRunningModeEnum = chip::app::Clusters::Thermostat::ThermostatRunningModeEnum;
using SetpointChangeSourceEnum  = chip::app::Clusters::Thermostat::SetpointChangeSourceEnum;
using FanModeEnum               = chip::app::Clusters::FanControl::FanModeEnum;

struct ProjectorConfig {
    int16_t autoBandHalfWidthCentiC      = 50; ///< ± from autoSetpoint
    int16_t runningModeHysteresisCentiC  = 50; ///< deadzone around setpoint
};

/// Complete cluster-attribute projection of LogicalACState. Field-by-field
/// equality on this struct is what produces the dirty-attribute set.
struct ProjectedClusterState {
    // OnOff cluster
    bool onOff;

    // Thermostat cluster
    SystemModeEnum            systemMode;
    int16_t                   occupiedHeatingSetpoint;
    int16_t                   occupiedCoolingSetpoint;
    ThermostatRunningModeEnum runningMode;
    std::optional<int16_t>    localTemperature;   // nullopt → cluster reports null
    std::optional<int16_t>    outdoorTemperature; // nullopt → cluster reports null
    SetpointChangeSourceEnum  setpointSource;

    // FanControl cluster
    FanSpeed                  speedSetting;       // nullopt → cluster reports null
    FanModeEnum               fanMode;
    uint8_t                   speedCurrent;       // mid-range while Auto

    // RelativeHumidityMeasurement cluster
    std::optional<uint16_t>   humidityCentiPercent; // nullopt → cluster reports null

    // BridgedDeviceBasicInformation
    bool                      reachable;

    // No defaulted comparison: we're on C++17 and Catch2 doesn't need it.
    // diffProjections compares field-by-field directly.
};

class Projector {
public:
    explicit Projector(ProjectorConfig cfg = {}) : mConfig(cfg) {}

    /// Full projection. Used by the reconciler's diff.
    ProjectedClusterState project(const LogicalACState&) const;

    // Per-attribute helpers for AAI Read paths. Each is the value the
    // matching cluster Accessor::Get would return if the data lived in
    // the cluster server's RAM.
    bool                          projectedOnOff(const LogicalACState&) const;
    SystemModeEnum                projectedSystemMode(const LogicalACState&) const;
    int16_t                       projectedOccupiedHeatingSetpoint(const LogicalACState&) const;
    int16_t                       projectedOccupiedCoolingSetpoint(const LogicalACState&) const;
    ThermostatRunningModeEnum     projectedRunningMode(const LogicalACState&) const;
    std::optional<int16_t>        projectedLocalTemperature(const LogicalACState&) const;
    std::optional<int16_t>        projectedOutdoorTemperature(const LogicalACState&) const;
    SetpointChangeSourceEnum      projectedSetpointSource(const LogicalACState&) const;
    FanSpeed                      projectedSpeedSetting(const LogicalACState&) const;
    FanModeEnum                   projectedFanMode(const LogicalACState&) const;
    uint8_t                       projectedSpeedCurrent(const LogicalACState&) const;
    std::optional<uint16_t>       projectedHumidityCentiPercent(const LogicalACState&) const;
    bool                          projectedReachable(const LogicalACState&) const;

    const ProjectorConfig& config() const { return mConfig; }

private:
    ProjectorConfig mConfig;

    ThermostatRunningModeEnum runningModeFromValve(bool refrigerantFlowing, const LogicalACState&) const;
    ThermostatRunningModeEnum runningModeFromTemperature(const LogicalACState&) const;
};

/// Field-by-field diff of two projections. Returns the cluster-attribute
/// paths that differ, on the given endpoint. Order is stable for testability.
std::vector<MatterAttributePath>
diffProjections(const ProjectedClusterState& before,
                const ProjectedClusterState& after,
                chip::EndpointId             endpoint);

} // namespace sync
