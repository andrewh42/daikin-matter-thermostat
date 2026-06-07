/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#include "reconciler.h"

namespace sync {

using SystemModeEnum = chip::app::Clusters::Thermostat::SystemModeEnum;
using FanModeEnum    = chip::app::Clusters::FanControl::FanModeEnum;
using Source         = ObservationSource;

// ─── Static enum routing helpers ──────────────────────────────────────────────

OperatingMode Reconciler::systemModeToOperatingMode(SystemModeEnum m)
{
    switch (m) {
    case SystemModeEnum::kCool:          return OperatingMode::Cool;
    case SystemModeEnum::kHeat:          return OperatingMode::Heat;
    case SystemModeEnum::kEmergencyHeat: return OperatingMode::Heat;
    case SystemModeEnum::kDry:           return OperatingMode::Dry;
    case SystemModeEnum::kFanOnly:       return OperatingMode::FanOnly;
    case SystemModeEnum::kPrecooling:    return OperatingMode::Cool;
    case SystemModeEnum::kAuto:
    default:                             return OperatingMode::Auto;
    }
}

SystemModeEnum Reconciler::operatingModeToSystemMode(OperatingMode m)
{
    switch (m) {
    case OperatingMode::Cool:         return SystemModeEnum::kCool;
    case OperatingMode::Heat:         return SystemModeEnum::kHeat;
    case OperatingMode::Dry:          return SystemModeEnum::kDry;
    case OperatingMode::FanOnly:      return SystemModeEnum::kFanOnly;
    case OperatingMode::Auto:
    case OperatingMode::Auto_Cooling:
    case OperatingMode::Auto_Heating: return SystemModeEnum::kAuto;
    default:                          return SystemModeEnum::kAuto;
    }
}

FanMode Reconciler::fanSpeedToS21FanMode(const FanSpeed& s)
{
    if (!s.has_value()) return FanMode::Auto;
    switch (*s) {
    case FanLevel::Quiet:   return FanMode::Quiet;
    case FanLevel::Low:     return FanMode::Low;
    case FanLevel::MidLow:  return FanMode::MidLow;
    case FanLevel::Medium:  return FanMode::Medium;
    case FanLevel::MidHigh: return FanMode::MidHigh;
    case FanLevel::High:    return FanMode::High;
    }
    return FanMode::Auto; // unreachable; silences -Wreturn-type
}

FanSpeed Reconciler::s21FanModeToSpeedSetting(FanMode m)
{
    switch (m) {
    case FanMode::Auto:    return std::nullopt;
    case FanMode::Quiet:   return FanLevel::Quiet;
    case FanMode::Low:     return FanLevel::Low;
    case FanMode::MidLow:  return FanLevel::MidLow;
    case FanMode::Medium:  return FanLevel::Medium;
    case FanMode::MidHigh: return FanLevel::MidHigh;
    case FanMode::High:    return FanLevel::High;
    }
    return std::nullopt; // unreachable; silences -Wreturn-type
}

// ─── Construction ─────────────────────────────────────────────────────────────

Reconciler::Reconciler(LogicalACState& state, TimeSource& time, ReconcilerConfig config)
    : mState(state), mTime(time), mConfig(config), mProjector(config.projector)
{
}

// ─── Guard window ─────────────────────────────────────────────────────────────

bool Reconciler::intentPassesGuard(int64_t lastDeviceObservationMs,
                                   bool valueDiffersFromObserved) const
{
    if (!valueDiffersFromObserved) return true; // nothing to conflict over
    if (lastDeviceObservationMs == 0) return true; // never observed from device
    const int64_t elapsed = mTime.millis() - lastDeviceObservationMs;
    return elapsed >= mConfig.deviceWinsWindowMs;
}

// ─── Per-intent application ───────────────────────────────────────────────────

void Reconciler::apply(const SetOnOffIntent& i)
{
    if (!intentPassesGuard(mLastDeviceObserved.onOff,
                           i.value != mState.onOff.observed())) return;
    mState.onOff.setDesired(i.value);
}

void Reconciler::apply(const SetSystemModeIntent& i)
{
    if (!intentPassesGuard(mLastDeviceObserved.mode,
                           i.value != mState.mode.observed())) return;
    mState.mode.setDesired(i.value);
}

void Reconciler::apply(const SetOccupiedHeatingSetpointIntent& i)
{
    switch (mState.mode.observed()) {
    case SystemModeEnum::kHeat:
    case SystemModeEnum::kEmergencyHeat:
        if (!intentPassesGuard(mLastDeviceObserved.heatSetpoint,
                               i.value != mState.heatSetpoint.observed())) return;
        mState.heatSetpoint.setDesired(i.value);
        return;

    case SystemModeEnum::kAuto: {
        // Auto-band heat-edge edit. New centre = midpoint of (current
        // projected cool edge, new heat edge). If both edges move by the
        // same delta in two intents, the net is a band translation
        // (modulo two-step centre drift — see Phase 4 atomic buffer).
        const int16_t projectedCool = static_cast<int16_t>(
            mState.autoSetpoint.desired() + mProjector.config().autoBandHalfWidthCentiC);
        const int16_t newCentre = static_cast<int16_t>((projectedCool + i.value) / 2);
        if (!intentPassesGuard(mLastDeviceObserved.autoSetpoint,
                               newCentre != mState.autoSetpoint.observed())) return;
        mState.autoSetpoint.setDesired(newCentre);
        return;
    }

    default:
        // Inactive-mode shadow write: stored so a mode flip back to Heat
        // lands on the controller's preferred value.
        mState.heatSetpoint.setDesired(i.value);
        return;
    }
}

void Reconciler::apply(const SetOccupiedCoolingSetpointIntent& i)
{
    switch (mState.mode.observed()) {
    case SystemModeEnum::kCool:
    case SystemModeEnum::kPrecooling:
        if (!intentPassesGuard(mLastDeviceObserved.coolSetpoint,
                               i.value != mState.coolSetpoint.observed())) return;
        mState.coolSetpoint.setDesired(i.value);
        return;

    case SystemModeEnum::kAuto: {
        const int16_t projectedHeat = static_cast<int16_t>(
            mState.autoSetpoint.desired() - mProjector.config().autoBandHalfWidthCentiC);
        const int16_t newCentre = static_cast<int16_t>((i.value + projectedHeat) / 2);
        if (!intentPassesGuard(mLastDeviceObserved.autoSetpoint,
                               newCentre != mState.autoSetpoint.observed())) return;
        mState.autoSetpoint.setDesired(newCentre);
        return;
    }

    default:
        // Inactive-mode shadow write: stored so a mode flip back to Cool
        // lands on the controller's preferred value.
        mState.coolSetpoint.setDesired(i.value);
        return;
    }
}

void Reconciler::apply(const SetSpeedSettingIntent& i)
{
    if (!intentPassesGuard(mLastDeviceObserved.fan,
                           i.value != mState.fan.observed())) return;
    mState.fan.setDesired(i.value);
}

// ─── Public entry points ──────────────────────────────────────────────────────

AppliedChange Reconciler::applyIntent(const WriteIntent& intent)
{
    const auto before = mProjector.project(mState);
    std::visit([this](const auto& specific) { apply(specific); }, intent);
    const auto after  = mProjector.project(mState);

    return {diffProjections(before, after, mConfig.endpoint), pendingCommand()};
}

AppliedChange Reconciler::applyAtomicBundle(const std::vector<WriteIntent>& intents)
{
    const auto before = mProjector.project(mState);

    // Auto-band atomic edit: in Auto mode, a (heat, cool) pair collapses
    // to autoSetpoint = midpoint. We use the LAST heat/cool intent in
    // bundle order (Matter semantics: later writes shadow earlier ones).
    const auto* lastHeat = static_cast<const SetOccupiedHeatingSetpointIntent*>(nullptr);
    const auto* lastCool = static_cast<const SetOccupiedCoolingSetpointIntent*>(nullptr);
    for (const auto& intent : intents) {
        if (auto* h = std::get_if<SetOccupiedHeatingSetpointIntent>(&intent)) lastHeat = h;
        if (auto* c = std::get_if<SetOccupiedCoolingSetpointIntent>(&intent)) lastCool = c;
    }

    const bool collapseAutoBand =
        (mState.mode.observed() == SystemModeEnum::kAuto) &&
        lastHeat != nullptr && lastCool != nullptr;

    if (collapseAutoBand) {
        const int16_t newCentre = static_cast<int16_t>((lastHeat->value + lastCool->value) / 2);
        if (intentPassesGuard(mLastDeviceObserved.autoSetpoint,
                              newCentre != mState.autoSetpoint.observed())) {
            mState.autoSetpoint.setDesired(newCentre);
        }
    }

    // Apply every other intent through the normal path. Heat/Cool intents
    // are skipped iff we just collapsed them above.
    for (const auto& intent : intents) {
        if (collapseAutoBand) {
            if (std::holds_alternative<SetOccupiedHeatingSetpointIntent>(intent)) continue;
            if (std::holds_alternative<SetOccupiedCoolingSetpointIntent>(intent)) continue;
        }
        std::visit([this](const auto& specific) { apply(specific); }, intent);
    }

    const auto after = mProjector.project(mState);
    return {diffProjections(before, after, mConfig.endpoint), pendingCommand()};
}

AppliedChange Reconciler::applyObservation(const S21State& obs)
{
    const auto before = mProjector.project(mState);
    const int64_t now = mTime.millis();

    mState.onOff.applyObservation(obs.onOff, Source::Device);
    mLastDeviceObserved.onOff = now;

    const auto observedSysMode = operatingModeToSystemMode(obs.operatingMode);
    mState.mode.applyObservation(observedSysMode, Source::Device);
    mLastDeviceObserved.mode = now;

    // The setpoint observation routes to the twin matching the *observed*
    // mode; the other-mode shadows are untouched.
    switch (obs.operatingMode) {
    case OperatingMode::Cool:
        mState.coolSetpoint.applyObservation(obs.setpointCelsius, Source::Device);
        mLastDeviceObserved.coolSetpoint = now;
        break;
    case OperatingMode::Heat:
        mState.heatSetpoint.applyObservation(obs.setpointCelsius, Source::Device);
        mLastDeviceObserved.heatSetpoint = now;
        break;
    case OperatingMode::Auto:
    case OperatingMode::Auto_Cooling:
    case OperatingMode::Auto_Heating:
        mState.autoSetpoint.applyObservation(obs.setpointCelsius, Source::Device);
        mLastDeviceObserved.autoSetpoint = now;
        break;
    case OperatingMode::Dry:
    case OperatingMode::FanOnly:
        break; // no setpoint-bearing twin
    }

    const auto observedFan = s21FanModeToSpeedSetting(obs.fanMode);
    mState.fan.applyObservation(observedFan, Source::Device);
    mLastDeviceObserved.fan = now;

    mState.indoorTemp.applyObservation(obs.indoorTemperatureCelsius, Source::Device);
    mState.outdoorTemp.applyObservation(obs.outdoorTemperatureCelsius, Source::Device);
    mState.humidity.applyObservation(obs.indoorRelativeHumidityPercent, Source::Device);

    // A successful poll proves the link is up.
    mState.reachable.applyObservation(true, Source::Device);

    const auto after = mProjector.project(mState);
    AppliedChange change{diffProjections(before, after, mConfig.endpoint), std::nullopt};
    // A fresh observation can clear an in-flight and unblock a queued
    // controller intent — check pendingCommand even on the observation path.
    change.sendCommand = pendingCommand();
    return change;
}

std::optional<S21OperationCommand> Reconciler::pendingCommand() const
{
    const bool anyDirty =
        mState.onOff.dirty() ||
        mState.mode.dirty()  ||
        mState.heatSetpoint.dirty() ||
        mState.coolSetpoint.dirty() ||
        mState.autoSetpoint.dirty() ||
        mState.fan.dirty();

    if (!anyDirty) return std::nullopt;

    S21OperationCommand cmd;
    cmd.onOff         = mState.onOff.desired();
    cmd.operatingMode = systemModeToOperatingMode(mState.mode.desired());

    switch (mState.mode.desired()) {
    case SystemModeEnum::kCool:
    case SystemModeEnum::kPrecooling:
        cmd.setpointCelsius = mState.coolSetpoint.desired();
        break;
    case SystemModeEnum::kHeat:
    case SystemModeEnum::kEmergencyHeat:
        cmd.setpointCelsius = mState.heatSetpoint.desired();
        break;
    case SystemModeEnum::kAuto:
        cmd.setpointCelsius = mState.autoSetpoint.desired();
        break;
    case SystemModeEnum::kDry:
    case SystemModeEnum::kFanOnly:
    default:
        cmd.setpointCelsius = mState.autoSetpoint.desired();
        break;
    }
    cmd.fanMode = fanSpeedToS21FanMode(mState.fan.desired());

    if (mLastSentCommand.has_value() && *mLastSentCommand == cmd) return std::nullopt;
    return cmd;
}

void Reconciler::onCommandSent(const S21OperationCommand& cmd)
{
    mLastSentCommand = cmd;
    mState.onOff.promoteDesiredToInFlight();
    mState.mode.promoteDesiredToInFlight();
    switch (mState.mode.desired()) {
    case SystemModeEnum::kCool:
    case SystemModeEnum::kPrecooling:
        mState.coolSetpoint.promoteDesiredToInFlight();
        break;
    case SystemModeEnum::kHeat:
    case SystemModeEnum::kEmergencyHeat:
        mState.heatSetpoint.promoteDesiredToInFlight();
        break;
    case SystemModeEnum::kAuto:
        mState.autoSetpoint.promoteDesiredToInFlight();
        break;
    default:
        break;
    }
    mState.fan.promoteDesiredToInFlight();
}

void Reconciler::onCommandFailed()
{
    // No clean way to "un-promote" in-flight; let the next observation
    // resolve it. Clearing mLastSentCommand re-arms pendingCommand so the
    // pump can retry once an observation arrives.
    mLastSentCommand.reset();
}

} // namespace sync
