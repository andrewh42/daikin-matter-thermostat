/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#include "reconciler.h"

namespace sync {

using Source = ObservationSource;

// ─── Static enum routing helpers ──────────────────────────────────────────────

OperatingMode Reconciler::operationalModeToS21OperatingMode(OperationalMode m)
{
    switch (m) {
    case OperationalMode::Cool:    return OperatingMode::Cool;
    case OperationalMode::Heat:    return OperatingMode::Heat;
    case OperationalMode::Dry:     return OperatingMode::Dry;
    case OperationalMode::FanOnly: return OperatingMode::FanOnly;
    case OperationalMode::Auto:    return OperatingMode::Auto;
    }
    return OperatingMode::Auto; // unreachable; silences -Wreturn-type
}

OperationalMode Reconciler::s21OperatingModeToOperationalMode(OperatingMode m)
{
    switch (m) {
    case OperatingMode::Cool:         return OperationalMode::Cool;
    case OperatingMode::Heat:         return OperationalMode::Heat;
    case OperatingMode::Dry:          return OperationalMode::Dry;
    case OperatingMode::FanOnly:      return OperationalMode::FanOnly;
    case OperatingMode::Auto:
    case OperatingMode::Auto_Cooling:
    case OperatingMode::Auto_Heating: return OperationalMode::Auto;
    }
    return OperationalMode::Auto; // unreachable; silences -Wreturn-type
}

std::optional<RunningMode> Reconciler::s21OperatingModeDirectionHint(OperatingMode m)
{
    switch (m) {
    case OperatingMode::Auto_Cooling: return RunningMode::Cooling;
    case OperatingMode::Auto_Heating: return RunningMode::Heating;
    case OperatingMode::Cool:
    case OperatingMode::Heat:
    case OperatingMode::Dry:
    case OperatingMode::FanOnly:
    case OperatingMode::Auto:         return std::nullopt;
    }
    return std::nullopt;
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

// ─── RunningMode fusion ───────────────────────────────────────────────────────

RunningMode Reconciler::fuseRunningMode(bool                onOff,
                                       OperatingMode       s21OperatingMode,
                                       std::optional<bool> refrigerantValveOpen) const
{
    if (!onOff) return RunningMode::Off;

    // 1. The S21 operatingMode value itself carries direction info for
    //    Auto_Cooling/Auto_Heating. That's the most authoritative signal.
    if (const auto hint = s21OperatingModeDirectionHint(s21OperatingMode); hint.has_value()) {
        return *hint;
    }

    const auto opMode = s21OperatingModeToOperationalMode(s21OperatingMode);

    // 2. Refrigerant valve closed → not actively running, regardless of mode.
    if (refrigerantValveOpen.has_value() && !*refrigerantValveOpen) {
        return RunningMode::Off;
    }

    // 3. Refrigerant valve open: direction is unambiguous for Cool/Heat;
    //    Auto needs indoor temp to disambiguate.
    if (refrigerantValveOpen.has_value() && *refrigerantValveOpen) {
        switch (opMode) {
        case OperationalMode::Cool: return RunningMode::Cooling;
        case OperationalMode::Heat: return RunningMode::Heating;
        case OperationalMode::Auto: {
            const auto& indoorOpt = mState.indoorTemp.observed();
            const int16_t sp      = mState.autoSetpoint.observed();
            const bool isHeating  = indoorOpt.has_value() && *indoorOpt < sp;
            return isHeating ? RunningMode::Heating : RunningMode::Cooling;
        }
        case OperationalMode::Dry:
        case OperationalMode::FanOnly:
        default:                    return RunningMode::Off;
        }
    }

    // 4. No valve info: fall back to indoor vs setpoint hysteresis.
    const auto& indoorOpt = mState.indoorTemp.observed();
    if (!indoorOpt.has_value()) return RunningMode::Off;
    const int16_t indoor = *indoorOpt;
    const int16_t hyst   = mConfig.runningModeHysteresisCentiC;

    switch (opMode) {
    case OperationalMode::Cool: {
        const int16_t sp = mState.coolSetpoint.observed();
        return (sp + hyst >= indoor) ? RunningMode::Off : RunningMode::Cooling;
    }
    case OperationalMode::Heat: {
        const int16_t sp = mState.heatSetpoint.observed();
        return (sp - hyst <= indoor) ? RunningMode::Off : RunningMode::Heating;
    }
    case OperationalMode::Auto: {
        const int16_t sp = mState.autoSetpoint.observed();
        if (indoor > sp + hyst) return RunningMode::Cooling;
        if (indoor < sp - hyst) return RunningMode::Heating;
        return RunningMode::Off;
    }
    case OperationalMode::Dry:
    case OperationalMode::FanOnly:
    default:                    return RunningMode::Off;
    }
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
    // Apply power first; mode write follows only when power-on. A power-off
    // intent retains the existing mode shadow (so a controller flipping
    // SystemMode kOff → kCool later lands on the previous setpoint set).
    if (intentPassesGuard(mLastDeviceObserved.onOff,
                          i.power != mState.onOff.observed())) {
        mState.onOff.setDesired(i.power);
    }
    if (i.power && intentPassesGuard(mLastDeviceObserved.mode,
                                     i.mode != mState.mode.observed())) {
        mState.mode.setDesired(i.mode);
    }
}

void Reconciler::apply(const SetOccupiedHeatingSetpointIntent& i)
{
    switch (mState.mode.observed()) {
    case OperationalMode::Heat:
        if (!intentPassesGuard(mLastDeviceObserved.heatSetpoint,
                               i.value != mState.heatSetpoint.observed())) return;
        mState.heatSetpoint.setDesired(i.value);
        return;

    case OperationalMode::Auto: {
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
    case OperationalMode::Cool:
        if (!intentPassesGuard(mLastDeviceObserved.coolSetpoint,
                               i.value != mState.coolSetpoint.observed())) return;
        mState.coolSetpoint.setDesired(i.value);
        return;

    case OperationalMode::Auto: {
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

OperationalChange Reconciler::applyIntent(const WriteIntent& intent)
{
    const auto before = mProjector.project(mState);
    std::visit([this](const auto& specific) { apply(specific); }, intent);
    const auto after  = mProjector.project(mState);

    return {diffProjections(before, after), pendingCommand()};
}

OperationalChange Reconciler::applyIntentBundle(const std::vector<WriteIntent>& intents)
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
        (mState.mode.observed() == OperationalMode::Auto) &&
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
    return {diffProjections(before, after), pendingCommand()};
}

OperationalChange Reconciler::applyOperationalObservation(
    const S21OperationalObservation& obs)
{
    const auto before = mProjector.project(mState);
    const int64_t now = mTime.millis();

    mState.onOff.applyObservation(obs.onOff, Source::Device);
    mLastDeviceObserved.onOff = now;

    const auto observedMode = s21OperatingModeToOperationalMode(obs.operatingMode);
    mState.mode.applyObservation(observedMode, Source::Device);
    mLastDeviceObserved.mode = now;

    // The setpoint observation routes to the twin matching the *observed*
    // mode; the other-mode shadows are untouched.
    switch (observedMode) {
    case OperationalMode::Cool:
        mState.coolSetpoint.applyObservation(obs.setpointCelsius, Source::Device);
        mLastDeviceObserved.coolSetpoint = now;
        break;
    case OperationalMode::Heat:
        mState.heatSetpoint.applyObservation(obs.setpointCelsius, Source::Device);
        mLastDeviceObserved.heatSetpoint = now;
        break;
    case OperationalMode::Auto:
        mState.autoSetpoint.applyObservation(obs.setpointCelsius, Source::Device);
        mLastDeviceObserved.autoSetpoint = now;
        break;
    case OperationalMode::Dry:
    case OperationalMode::FanOnly:
        break; // no setpoint-bearing twin
    }

    const auto observedFan = s21FanModeToSpeedSetting(obs.fanMode);
    mState.fan.applyObservation(observedFan, Source::Device);
    mLastDeviceObserved.fan = now;

    mState.refrigerantValveOpen.applyObservation(obs.refrigerantValveOpen);

    // Fuse runningMode at observation time: all the signals we need are
    // either in this observation or already in state. The projector reads
    // state.runningMode directly and doesn't redo the inference.
    mState.runningMode.applyObservation(
        fuseRunningMode(obs.onOff, obs.operatingMode, obs.refrigerantValveOpen));

    // A successful op observation proves the link is up. Env doesn't run
    // every tick, so reachable is anchored on the op heartbeat alone.
    mState.reachable.applyObservation(true);

    const auto after = mProjector.project(mState);
    OperationalChange change{diffProjections(before, after), std::nullopt};
    // A fresh observation can clear an in-flight and unblock a queued
    // controller intent — check pendingCommand even on the observation path.
    change.sendCommand = pendingCommand();
    return change;
}

EnvironmentalChange Reconciler::applyEnvironmentalObservation(
    const S21EnvironmentalObservation& obs)
{
    const auto before = mProjector.project(mState);

    mState.indoorTemp.applyObservation(obs.indoorTemperatureCelsius);
    mState.outdoorTemp.applyObservation(obs.outdoorTemperatureCelsius);
    mState.humidity.applyObservation(obs.indoorRelativeHumidityPercent);

    const auto after = mProjector.project(mState);
    return {diffProjections(before, after)};
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
    cmd.operatingMode = operationalModeToS21OperatingMode(mState.mode.desired());

    switch (mState.mode.desired()) {
    case OperationalMode::Cool:
        cmd.setpointCelsius = mState.coolSetpoint.desired();
        break;
    case OperationalMode::Heat:
        cmd.setpointCelsius = mState.heatSetpoint.desired();
        break;
    case OperationalMode::Auto:
    case OperationalMode::Dry:
    case OperationalMode::FanOnly:
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
    case OperationalMode::Cool:
        mState.coolSetpoint.promoteDesiredToInFlight();
        break;
    case OperationalMode::Heat:
        mState.heatSetpoint.promoteDesiredToInFlight();
        break;
    case OperationalMode::Auto:
        mState.autoSetpoint.promoteDesiredToInFlight();
        break;
    case OperationalMode::Dry:
    case OperationalMode::FanOnly:
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
