/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * LogicalACState
 * --------------
 * The bridge's canonical AC state. Reconcilable fields (onOff, mode,
 * setpoints, fan) are TwinFields that track observed / desired / in-flight
 * independently, so the reconciler can decide per-attribute whether to
 * send a device command or mark a cluster attribute dirty.
 *
 * Observation-only fields (indoor/outdoor temperature, humidity,
 * refrigerantValveOpen, reachable) are SensorFields. Matter never writes
 * them, so the desired/in-flight halves of a twin would be dead weight;
 * SensorField encodes that asymmetry in the type system.
 *
 * Two further things to know:
 *
 *  - The Auto setpoint is the *real* device target temperature in Auto
 *    mode. The Matter cluster exposes a band as (autoSetpoint − δ,
 *    autoSetpoint + δ); the projector synthesises that band on read, and
 *    the reconciler collapses controller edits to either edge back to a
 *    single autoSetpoint update on write. See sync-analysis §4.6 and the
 *    Phase 3 projector tests.
 *
 *  - The class is pure data + tiny accessors. The reconciler owns all
 *    policy. This keeps the type host-testable without dragging in CHIP
 *    types or Zephyr.
 */
#pragma once

#include "sensor_field.h"
#include "twin_field.h"

#include <app-common/zap-generated/attributes/Accessors.h>

#include <cstdint>
#include <optional>

namespace sync {

using SystemModeEnum = chip::app::Clusters::Thermostat::SystemModeEnum;

/// Named S21 fan-speed levels. Underlying values match the Matter
/// FanControl::SpeedSetting wire format (1..SpeedMax), so the AAI's
/// decoded `uint8_t` is a direct `static_cast` to FanLevel and back.
/// SpeedMax for our device is 6 (see ZAP); values outside 1..6 should
/// be rejected at the AAI boundary, not represented here.
enum class FanLevel : uint8_t {
    Quiet   = 1,
    Low     = 2,
    MidLow  = 3,
    Medium  = 4,
    MidHigh = 5,
    High    = 6,
};

/// nullopt → Auto (the Matter SpeedSetting null state). The optional
/// wrapper preserves nullable cluster semantics; the enum eliminates
/// the 1..6 magic numbers from the reconciler and projector switches.
using FanSpeed = std::optional<FanLevel>;

/// Boot-time defaults. Held outside LogicalACState so the same struct can
/// drive both the production constructor and per-test setup without
/// surprising defaulted-member behaviour.
struct LogicalACStateDefaults {
    bool           onOff         = false;
    SystemModeEnum mode          = SystemModeEnum::kOff;
    int16_t        heatSetpoint  = 2000; ///< 0.01 °C
    int16_t        coolSetpoint  = 2500; ///< 0.01 °C
    int16_t        autoSetpoint  = 2200; ///< 0.01 °C — midpoint of Auto band
    FanSpeed       fan           = std::nullopt;
    std::optional<int16_t> indoorTemp    = std::nullopt;
    std::optional<int16_t> outdoorTemp   = std::nullopt;
    std::optional<uint8_t> humidity      = std::nullopt;
    std::optional<bool>    refrigerantValveOpen = std::nullopt;
    bool           reachable     = false;
};

struct LogicalACState {
    explicit LogicalACState(const LogicalACStateDefaults& d = {})
        : onOff(d.onOff),
          mode(d.mode),
          heatSetpoint(d.heatSetpoint),
          coolSetpoint(d.coolSetpoint),
          autoSetpoint(d.autoSetpoint),
          fan(d.fan),
          indoorTemp(d.indoorTemp),
          outdoorTemp(d.outdoorTemp),
          humidity(d.humidity),
          refrigerantValveOpen(d.refrigerantValveOpen),
          reachable(d.reachable)
    {
    }

    TwinField<bool>           onOff;
    TwinField<SystemModeEnum> mode;
    TwinField<int16_t>        heatSetpoint;
    TwinField<int16_t>        coolSetpoint;
    TwinField<int16_t>        autoSetpoint;
    TwinField<FanSpeed>       fan;

    SensorField<std::optional<int16_t>> indoorTemp;
    SensorField<std::optional<int16_t>> outdoorTemp;
    SensorField<std::optional<uint8_t>> humidity;
    SensorField<std::optional<bool>>    refrigerantValveOpen;
    SensorField<bool>                   reachable;

    /// The setpoint twin currently in charge of the device's target T,
    /// given a system mode. Auto returns the auto-shadow; Cool/Heat return
    /// their respective edges. Modes without a setpoint concept (Off, Dry,
    /// FanOnly, …) return the Auto shadow so callers always get *something*
    /// without UB; the reconciler is expected not to consult this for those
    /// modes.
    TwinField<int16_t>& activeSetpoint(SystemModeEnum m)
    {
        switch (m) {
        case SystemModeEnum::kCool:        return coolSetpoint;
        case SystemModeEnum::kHeat:        return heatSetpoint;
        case SystemModeEnum::kAuto:        return autoSetpoint;
        default:                           return autoSetpoint;
        }
    }
    const TwinField<int16_t>& activeSetpoint(SystemModeEnum m) const
    {
        return const_cast<LogicalACState*>(this)->activeSetpoint(m);
    }
};

} // namespace sync
