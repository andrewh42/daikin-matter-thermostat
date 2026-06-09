/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#pragma once

#include <optional>
#include <utility>

namespace sync {

/// Provenance of the most recent observation applied to a twin. Lifted out
/// of TwinField<T> so all twins share a single enum type (and tests don't
/// have to qualify by instantiation).
enum class ObservationSource { Boot, Device, Matter };

/**
 * TwinField<T> is a single-value algebra for a piece of bridge state with
 * three views:
 *
 *   - observed:  what the device most recently reported
 *   - desired:   what the controller (or boot defaults) wants
 *   - inFlight:  the value the bridge has just sent to the device and is
 *                waiting to see echoed back, if any
 *
 * The point is to make echo-suppression and external-override decisions
 * mechanically obvious. Callers don't reason about "is this poll stale" —
 * they call applyObservation() and the algebra does the right thing.
 *
 * Designed to be header-only and free of CHIP/Zephyr dependencies so the
 * reconciler that owns these fields stays host-testable.
 */
template <typename T>
class TwinField {
public:
    using Source = ObservationSource;

    explicit TwinField(T initial)
        : mObserved(initial), mDesired(initial), mInFlight(),
          mLastSource(Source::Boot), mAttribution(Source::Boot)
    {
    }

    const T&                observed()          const { return mObserved; }
    const T&                desired()           const { return mDesired; }
    const std::optional<T>& inFlight()          const { return mInFlight; }
    Source                  lastObservedSource() const { return mLastSource; }

    /// "Who effectively owns this twin's current value?" — distinct from
    /// lastObservedSource() because a controller-initiated write that the
    /// device confirms should still attribute to Matter, even though the
    /// confirming observation arrived from the device. Used by the
    /// Thermostat::SetpointChangeSource projection.
    ///
    /// Boot default is Boot; setDesired() (intent) sets it to Matter; an
    /// external observation that disconfirms an in-flight or arrives with
    /// no in-flight sets it to whatever source the observation cited
    /// (typically Device → Manual on the wire).
    Source                  attribution()        const { return mAttribution; }

    /// True when there is unsent controller intent. Compares desired against
    /// "what the device should currently believe": the in-flight value if a
    /// command is pending, otherwise the last observed value. This lets a
    /// controller supersede an in-flight write (e.g. Cool→Auto→Cool) — the
    /// new desired diverges from the in-flight even though it matches
    /// observed, and dirty() goes true so a new command is emitted.
    bool dirty() const
    {
        return mInFlight.has_value() ? (mDesired != *mInFlight)
                                     : (mDesired != mObserved);
    }

    /// Record a new controller intent.
    /// Does not clear an in-flight value: a controller may queue a new
    /// desired while waiting for the previous one to confirm. The reconciler
    /// decides whether to coalesce or to wait. Intents are always sourced
    /// from a Matter controller in this codebase (S21 polls flow through
    /// applyObservation, not setDesired).
    ///
    /// A no-op intent (value already equals desired) doesn't shift
    /// attribution, so a controller "asserting" a value it already shows
    /// doesn't generate a spurious SetpointChangeSource update.
    void setDesired(T value)
    {
        if (mDesired == value) return;
        mDesired     = std::move(value);
        mAttribution = Source::Matter;
    }

    /// Capture the current desired as the value about to be sent to the
    /// device. After this call, dirty() is false until the next setDesired()
    /// or an observation overrides things.
    void promoteDesiredToInFlight()
    {
        mInFlight = mDesired;
    }

    /// Process a value reported by the device (or any other observation
    /// source). Five-way branching keyed off (inFlight, value-vs-observed,
    /// value-vs-inFlight):
    ///
    ///   1. inFlight set, value == inFlight → confirmation. Clear inFlight;
    ///      observed = value. desired left alone (it matched inFlight, so
    ///      this is consistent).
    ///   2. inFlight set, value == observed (the pre-send value) → stale
    ///      poll: the device hasn't yet processed our command, or the poll
    ///      snapshot predates it. Keep inFlight; don't touch observed.
    ///   3. inFlight set, value ≠ inFlight and ≠ observed → disconfirmation:
    ///      the device clamped or an external panel intervened. Clear
    ///      inFlight; observed = value. desired LEFT ALONE so any controller
    ///      intent queued during the in-flight is preserved (dedup against
    ///      mLastSentCommand handles retry suppression).
    ///   4. no inFlight, value == observed → idempotent no-op other than
    ///      lastObservedSource refresh. desired untouched (which is the bug
    ///      fix: a stale poll matching pre-write observed cannot revert a
    ///      fresher Matter intent).
    ///   5. no inFlight, value ≠ observed → external change. observed =
    ///      desired = value (a remote panel turned the knob; reflect both
    ///      ways so nothing dirties).
    void applyObservation(T value, Source source)
    {
        mLastSource = source;

        if (mInFlight.has_value()) {
            if (value == *mInFlight) {                // 1: confirmation
                mInFlight.reset();
                mObserved = std::move(value);
                // attribution stays as set by setDesired (Source::Matter)
                // — the device honoured our write, so the controller is
                // still the rightful "source of this value".
            } else if (value == mObserved) {          // 2: stale poll
                // No mutation, including attribution.
            } else {                                  // 3: disconfirmation
                mInFlight.reset();
                mObserved = std::move(value);
                mAttribution = source; // device clamped or external panel
                // desired left alone — preserves any queued intent.
            }
            return;
        }

        if (value != mObserved) {                     // 5: external change
            mObserved = std::move(value);
            mDesired  = mObserved;
        }
        // Branches 4 and 5 both reaffirm `source` as the most recent
        // touch on this twin. (Branch 1's "controller-write confirmed"
        // case is handled above by leaving attribution as the prior
        // Matter intent set it.)
        mAttribution = source;
    }

private:
    T                mObserved;
    T                mDesired;
    std::optional<T> mInFlight;
    Source           mLastSource;
    Source           mAttribution;
};

} // namespace sync
