/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#pragma once

#include "fan_mapping.h"
#include "logical_ac_state.h"
#include "logical_attribute.h"
#include "operational_mode.h"
#include "twin_field.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace sync {

struct ProjectorConfig {
    int16_t autoBandHalfWidthCentiC = 50; ///< ± from autoSetpoint
};

/// Complete domain-side projection of LogicalACState. Field-by-field
/// equality on this struct is what produces the dirty-attribute set.
struct ProjectedClusterState {
    // OnOff
    bool                          onOff;

    // Thermostat
    OperationalMode               mode;
    int16_t                       occupiedHeatingSetpoint;
    int16_t                       occupiedCoolingSetpoint;
    RunningMode                   runningMode;
    std::optional<int16_t>        localTemperature;
    std::optional<int16_t>        outdoorTemperature;
    ObservationSource             setpointSource;

    // FanControl. The fan attributes are power-coupled: when the unit is
    // off the fan is off, so speedSetting/percentSetting read 0, fanMode
    // reads Off, and the Current pair read 0. speedSetting/percentSetting
    // are nullable: nullopt ⇔ Auto (null), 0 ⇔ Off, else the running value.
    std::optional<uint8_t>        speedSetting;
    FanModeCategory               fanMode;
    uint8_t                       speedCurrent;
    std::optional<uint8_t>        percentSetting;
    uint8_t                       percentCurrent;

    // RelativeHumidityMeasurement
    std::optional<uint16_t>       humidityCentiPercent;

    // BridgedDeviceBasicInformation
    bool                          reachable;
};

/**
 * Projector is a pure function from LogicalACState to the domain values
 * that subscribers will see.
 *
 * Two consumers:
 *
 *   - The reconciler diffs project(before) vs project(after) and emits a
 *     vector<LogicalAttribute> naming what changed.
 *
 *   - The AAI Read paths call per-attribute projectors directly and
 *     translate the domain value to CHIP at encode time
 *     (see aai_translation.h).
 *
 * Projector outputs are *domain* types throughout (`OperationalMode`,
 * `RunningMode`, `ObservationSource`, `FanSpeed`, `std::optional<T>`):
 * Matter cluster enums and `chip::app::DataModel::Nullable<T>` appear only
 * on the AAI side of the wall.
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
 * RunningMode comes from state.runningMode (fused at observation time by
 * the reconciler), with a !onOff override.
 *
 * Projections read TwinField::observed() (the "faithful UI" policy): a
 * controller's view tracks what the device has actually acknowledged, not
 * what we hope it will become. desired() leaks would confuse subscribers
 * during the inFlight window.
 */
class Projector {
public:
    explicit Projector(ProjectorConfig cfg = {}) : mConfig(cfg) {}

    /// Full projection. Used by the reconciler's diff.
    ProjectedClusterState project(const LogicalACState&) const;

    // Per-attribute helpers. Domain types throughout; the AAI Read paths
    // call sync_aai::toMatter… at encode time.
    bool                       projectedOnOff(const LogicalACState&) const;
    OperationalMode            projectedMode(const LogicalACState&) const;
    int16_t                    projectedOccupiedHeatingSetpoint(const LogicalACState&) const;
    int16_t                    projectedOccupiedCoolingSetpoint(const LogicalACState&) const;
    RunningMode                projectedRunningMode(const LogicalACState&) const;
    std::optional<int16_t>     projectedLocalTemperature(const LogicalACState&) const;
    std::optional<int16_t>     projectedOutdoorTemperature(const LogicalACState&) const;
    ObservationSource          projectedSetpointSource(const LogicalACState&) const;
    std::optional<uint8_t>     projectedSpeedSetting(const LogicalACState&) const;
    FanModeCategory            projectedFanMode(const LogicalACState&) const;
    uint8_t                    projectedSpeedCurrent(const LogicalACState&) const;
    std::optional<uint8_t>     projectedPercentSetting(const LogicalACState&) const;
    uint8_t                    projectedPercentCurrent(const LogicalACState&) const;
    std::optional<uint16_t>    projectedHumidityCentiPercent(const LogicalACState&) const;
    bool                       projectedReachable(const LogicalACState&) const;

    const ProjectorConfig& config() const { return mConfig; }

private:
    ProjectorConfig mConfig;
};

/// Field-by-field diff of two projections. Returns the LogicalAttribute
/// values that differ. Order is stable for testability.
///
/// SystemMode is derived from (onOff, mode), so the diff emits it whenever
/// either of those underlying fields changes.
std::vector<LogicalAttribute>
diffProjections(const ProjectedClusterState& before,
                const ProjectedClusterState& after);

} // namespace sync
