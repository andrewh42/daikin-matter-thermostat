/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#pragma once

#include "logical_ac_state.h"

#include <cstdint>

namespace sync {

/**
 * fan_mapping holds the pure, CHIP-free conversions between the bridge's
 * discrete fan model and the Matter FanControl cluster's speed/percent/mode
 * representations. Centralised here so the reconciler, projector and AAI all
 * agree on the (lossy) speed↔percent mapping and the speed→mode ranges.
 *
 * The device exposes 6 discrete fan levels (FanLevel 1..6) plus Auto
 * (FanSpeed == nullopt). "Off" is **not** represented here: this AC's fan
 * cannot stop independently of unit power, so FanControl "Off" maps to the
 * OnOff/power axis, not to a fan level (see the projector's power coupling
 * and the AAI write paths).
 *
 * Mapping rules follow Matter 1.5 FanControl §4.4.6.3.1 (Percent Rules) and
 * §4.4.6.6.1 (Speed Rules) for SpeedMax = 6 / FanModeSequence
 * OffLowMedHighAuto:
 *   - percent = floor(speed / SpeedMax · 100)
 *   - speed   = ceil(SpeedMax · percent · 0.01)
 *   - speed range → mode: 1-2 → Low, 3-4 → Medium, 5-6 → High, null → Auto.
 */

/// Maximum SpeedSetting value, matching the ZAP SpeedMax attribute. Single
/// source of truth for the 1..kFanSpeedMax range checks.
constexpr uint8_t kFanSpeedMax = 6;

/// Domain-side coarse fan mode. Kept CHIP-free (the projector diffs on it);
/// the AAI translates it to chip::…::FanModeEnum at the boundary.
enum class FanModeCategory : uint8_t { Off, Low, Medium, High, Auto };

/// Speed Rules: percent = floor(speed / SpeedMax · 100). 1→16 … 6→100.
inline uint8_t speedLevelToPercent(FanLevel level)
{
    return static_cast<uint8_t>((static_cast<uint16_t>(level) * 100) / kFanSpeedMax);
}

/// Percent Rules: speed = ceil(SpeedMax · percent · 0.01), clamped to
/// 1..kFanSpeedMax. Callers must not pass 0 (the 0/Off range is handled on
/// the power axis); clamping to 1 is a defensive floor.
inline FanLevel percentToSpeedLevel(uint8_t percent)
{
    uint32_t raw = (static_cast<uint32_t>(kFanSpeedMax) * percent + 99) / 100;
    if (raw < 1)            raw = 1;
    if (raw > kFanSpeedMax) raw = kFanSpeedMax;
    return static_cast<FanLevel>(raw);
}

/// Range-map a running fan setting to its coarse FanMode (Off excluded —
/// that comes from the power axis). null → Auto.
inline FanModeCategory fanModeOf(const FanSpeed& fan)
{
    if (!fan.has_value()) return FanModeCategory::Auto;
    switch (*fan) {
    case FanLevel::Quiet:
    case FanLevel::Low:     return FanModeCategory::Low;     // 1-2
    case FanLevel::MidLow:
    case FanLevel::Medium:  return FanModeCategory::Medium;  // 3-4
    case FanLevel::MidHigh:
    case FanLevel::High:    return FanModeCategory::High;     // 5-6
    }
    return FanModeCategory::Auto; // unreachable; silences -Wreturn-type
}

/// The representative speed a FanMode write lands on (any value in the range
/// is spec-legal; these align with the S21 named levels Low/Medium/High).
/// Only meaningful for Low/Medium/High; Off/Auto are handled by the caller.
inline FanLevel representativeSpeed(FanModeCategory cat)
{
    switch (cat) {
    case FanModeCategory::Low:    return FanLevel::Low;    // 2
    case FanModeCategory::Medium: return FanLevel::Medium; // 4
    case FanModeCategory::High:   return FanLevel::High;   // 6
    case FanModeCategory::Off:
    case FanModeCategory::Auto:   return FanLevel::Low;
    }
    return FanLevel::Low; // unreachable; silences -Wreturn-type
}

} // namespace sync
