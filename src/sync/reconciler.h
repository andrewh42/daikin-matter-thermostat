/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Reconciler
 * ----------
 * Central policy engine. Two entry points:
 *
 *   applyIntent(intent)        — controller (Matter) write arrived
 *   applyObservation(snapshot) — S21 poll completed
 *
 * Both mutate LogicalACState through the TwinField algebra and return an
 * AppliedChange describing what changed:
 *
 *   - dirtyAttributes: cluster attributes whose projected value differs
 *     after the call. Caller (Phase 7 pump) reports each to subscribers
 *     via MatterReportingAttributeChangeCallback.
 *
 *   - sendCommand: the next S21 setOperation() to dispatch, or nullopt if
 *     nothing should be sent (no dirty desireds, in-flight pending, or
 *     command identical to the last one we sent).
 *
 * Decisions the reconciler makes:
 *
 *   - Echo suppression / TOCTOU. A stale poll arriving after a controller
 *     write can no longer flip desired back, because the TwinField sees
 *     value == observed and treats it as idempotent. (Tested in Group A.)
 *
 *   - Auto-mode 3-slot shadow. In Auto mode the device's real target is a
 *     single number; the Matter cluster reports it as a band (auto ± δ).
 *     An edge edit recomputes the centre as midpoint(other-edge,
 *     new-edge); a both-edges-equal-delta edit translates the centre by
 *     that delta. (Tested in Group B.)
 *
 *   - Guard window. A Matter intent that arrives within
 *     `deviceWinsWindowMs` of a Device-source observation that disagrees
 *     with the intent is treated as a stale write predating the manual
 *     change and is dropped. (Tested in Group D.)
 *
 *   - Dedup against last sent command. If a recompute would re-emit the
 *     identical S21 command we just sent, suppress it. (Mirrors the
 *     existing AirConditionerManager::mLastSentCommand behaviour.)
 *
 * Threading: not thread-safe by itself. The Phase 7 wrapper owns a
 * single mutex around every call. The reconciler is otherwise pure.
 */
#pragma once

#include "logical_ac_state.h"
#include "matter_attribute_path.h"
#include "matter_to_s21_translator.h"
#include "projector.h"
#include "time_source.h"
#include "write_intent.h"

#include <optional>
#include <vector>

namespace sync {

struct ReconcilerConfig {
    chip::EndpointId endpoint           = 1;
    int64_t          deviceWinsWindowMs = 1000;   ///< Group D guard window
    ProjectorConfig  projector{};                 ///< Shared with the AAI Read paths.
};

struct AppliedChange {
    std::vector<MatterAttributePath>   dirtyAttributes;
    std::optional<S21OperationCommand> sendCommand;
};

class Reconciler {
public:
    Reconciler(LogicalACState& state, TimeSource& time, ReconcilerConfig config = {});

    /// Apply a controller-side intent. Updates state and returns the
    /// dirty-attribute set + any S21 command that should now be sent.
    AppliedChange applyIntent(const WriteIntent& intent);

    /// Apply a bundle of intents as if they arrived simultaneously. Used
    /// by AtomicTxn::commit to implement Matter AtomicRequest semantics
    /// without the centre drift that two non-atomic Auto-band edits would
    /// cause. Special-cases: in Auto mode, a (heat, cool) pair collapses
    /// to autoSetpoint = midpoint. Returns the same shape as applyIntent.
    AppliedChange applyAtomicBundle(const std::vector<WriteIntent>& intents);

    /// Apply a device-side observation (polled S21 snapshot). Updates state
    /// and returns the dirty-attribute set; sendCommand is normally nullopt
    /// because polls don't generate writes.
    AppliedChange applyObservation(const S21State& observation);

    /// Promote the corresponding twins to in-flight. Call after the S21
    /// command has been handed off to the data link.
    void onCommandSent(const S21OperationCommand& cmd);

    /// Clear any in-flight values without confirming them. Call when the
    /// data link reports the command failed (timeout/NAK). The desireds
    /// stay set, so the next pendingCommand() may retry.
    void onCommandFailed();

    /// What command should be sent next, given current twin state and the
    /// last-sent dedup memory. nullopt if nothing dirty or duplicate.
    std::optional<S21OperationCommand> pendingCommand() const;

    const ReconcilerConfig& config()    const { return mConfig; }
    const Projector&        projector() const { return mProjector; }

private:
    LogicalACState&                    mState;
    TimeSource&                        mTime;
    ReconcilerConfig                   mConfig;
    Projector                          mProjector;
    std::optional<S21OperationCommand> mLastSentCommand;

    // Per-mutable-twin timestamp of last Device-source observation; used by
    // the guard window. Sensor-only fields don't need this.
    struct LastDeviceTimes {
        int64_t onOff        = 0;
        int64_t mode         = 0;
        int64_t heatSetpoint = 0;
        int64_t coolSetpoint = 0;
        int64_t autoSetpoint = 0;
        int64_t fan          = 0;
    } mLastDeviceObserved;

    // Per-intent application helpers; one per WriteIntent alternative to
    // keep applyIntent() flat and the mode-aware Auto routing explicit.
    void apply(const SetOnOffIntent&);
    void apply(const SetSystemModeIntent&);
    void apply(const SetOccupiedHeatingSetpointIntent&);
    void apply(const SetOccupiedCoolingSetpointIntent&);
    void apply(const SetSpeedSettingIntent&);

    // Guard-window check for setpoints/onOff/mode/fan. Returns true if the
    // intent should be applied; false to drop it.
    bool intentPassesGuard(int64_t lastDeviceObservationMs, bool valueDiffersFromObserved) const;

    static OperatingMode  systemModeToOperatingMode(SystemModeEnum);
    static SystemModeEnum operatingModeToSystemMode(OperatingMode);
    static FanMode        fanSpeedToS21FanMode(const FanSpeed&);
    static FanSpeed       s21FanModeToSpeedSetting(FanMode);
};

} // namespace sync
