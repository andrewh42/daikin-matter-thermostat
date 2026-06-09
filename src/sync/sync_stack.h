/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * SyncStack — production singleton owning the bridge's canonical AC state
 * and serialising access from the two threads that touch it:
 *
 *   - The Matter event loop (AAI Read/Write paths)
 *   - The S21 work queue       (poll observations, command pump)
 *
 * Phase 5 (this file): instantiated by AppTask::Init. Provides:
 *   - per-attribute Read helpers used by the AAI subclasses;
 *   - mutating entry points (ApplyIntent, ApplyObservation, OnCommandSent)
 *     that grab a single internal mutex.
 *
 * Phase 7 will add a poll/command pump that uses these entry points to
 * replace AirConditionerManager's direct cluster dispatch.
 */
#pragma once

#include "atomic_buffer.h"
#include "changed_attributes_listener.h"
#include "logical_ac_state.h"
#include "matter_attribute_path.h"
#include "monotonic_time_source.h"
#include "projector.h"
#include "reconciler.h"
#include "sync_reader.h"
#include "write_intent.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <lib/core/CHIPError.h>

#include <optional>
#include <vector>
#include <zephyr/kernel.h>

namespace sync {

class SyncStack {
public:
    /// Maximum number of ChangedAttributesListeners that may be registered
    /// at once. Sized for: Matter reporter, LED indicator, optional shell
    /// observer, optional metrics. Bump if you need more.
    static constexpr size_t kMaxListeners = 4;

    static SyncStack& Instance();

    /// One-time construction of the LogicalACState / Reconciler / AtomicTxn
    /// triple and registration of the three AAI subclasses (Thermostat,
    /// OnOff, FanControl). After this, the AAI Read/Write paths route
    /// through us.
    CHIP_ERROR Init(chip::EndpointId endpoint = 1);

    /// Unregister AAIs and tear down state. Safe to call from Shutdown
    /// paths; no-op if Init was never called.
    void Shutdown();

    // ─── Mutating entry points (lock-acquiring) ──────────────────────────────

    /// Apply a controller write. Called from AAI Write, under the CHIP
    /// stack lock. Returns the resulting dirty-path set and the next S21
    /// command (if any) for the pump to dispatch.
    OperationalChange ApplyIntent(const WriteIntent& intent);

    /// Apply the operational half of a poll snapshot (onOff / mode /
    /// setpoint / fan / refrigerantValveOpen). Called from the S21 work
    /// queue every poll tick. Returns the dirty-path set + an optional
    /// follow-up command; the caller posts each path to the Matter event
    /// loop for MatterReportingAttributeChangeCallback under LockChipStack.
    OperationalChange ApplyOperationalObservation(const S21OperationalObservation& obs);

    /// Apply the environmental half of a poll snapshot (indoor/outdoor
    /// temperature, humidity). Called from the S21 work queue at the
    /// reduced sensor cadence (kS21EnvironmentalSensorReadTicks). Returns
    /// the dirty-path set only — env observations cannot produce a
    /// follow-up S21 command.
    EnvironmentalChange ApplyEnvironmentalObservation(const S21EnvironmentalObservation& obs);

    /// Promote desired→inFlight after a successful setOperation. Called
    /// from the S21 work queue.
    void OnCommandSent(const S21OperationCommand& cmd);

    /// Clear in-flight state after a failed (timeout/NAK) setOperation.
    void OnCommandFailed();

    /// Mark the data link as unreachable. The next successful poll flips
    /// it back via ApplyObservation. Used by Phase 8 supervisor.
    void NotifyLinkDown();

    /// The next S21 command to send, if any. Called by the Phase 7 pump.
    std::optional<S21OperationCommand> PendingCommand() const;

    /// Single-consumer pump signal. Called from inside ApplyIntent and
    /// ApplyObservation when those produce a pendingCommand. The pump uses
    /// it to wake itself without us needing to know about Zephyr work
    /// queues here. Pass nullptr to clear.
    using CommandPumpHandler = void (*)();
    void SetCommandPumpHandler(CommandPumpHandler handler) { mPumpHandler = handler; }

    /// Register/deregister a ChangedAttributesListener. Listeners are
    /// invoked outside SyncStack's mutex after every mutation that
    /// produces a non-empty dirty-attribute set; the order of invocation
    /// is registration order.
    ///
    /// Add is idempotent for the same pointer. Returns CHIP_ERROR_NO_MEMORY
    /// if the listener table is full (capacity == kMaxListeners). Remove
    /// is a silent no-op for an unknown pointer.
    CHIP_ERROR AddChangedAttributesListener(ChangedAttributesListener* listener);
    void       RemoveChangedAttributesListener(ChangedAttributesListener* listener);

    // ─── Atomic-request transaction (Matter AtomicRequest command) ───────────

    AtomicTxn::Status BeginAtomic();
    AtomicTxn::Status AtomicWrite(const WriteIntent& intent);
    OperationalChange     CommitAtomic();
    AtomicTxn::Status RollbackAtomic();

    // ─── Per-attribute projected reads ───────────────────────────────────────

    /// The thread-safe read facade used by AAI Read paths. Lifetime is
    /// tied to this SyncStack instance; the returned reference is invalid
    /// after Shutdown().
    const SyncReader& Reader() const { return *mReader; }

    /// Value-copy of the underlying twin state, taken under the internal
    /// lock. For debug surfaces (shell commands, logs) that want the raw
    /// observed/desired/inFlight triples rather than projected reads. The
    /// copy lets callers format outside the lock so shell I/O doesn't
    /// extend the mutex hold that ApplyObservation contends for.
    LogicalACState Snapshot() const;

    chip::EndpointId Endpoint() const { return mEndpoint; }

private:
    SyncStack() = default;
    ~SyncStack() = default;
    SyncStack(const SyncStack&)            = delete;
    SyncStack& operator=(const SyncStack&) = delete;

    // RAII mutex scope. Public to nested AAI helpers if needed, but in
    // practice every entry point acquires it itself.
    class LockGuard {
    public:
        explicit LockGuard(k_mutex& m) : mM(m) { k_mutex_lock(&m, K_FOREVER); }
        ~LockGuard()                            { k_mutex_unlock(&mM); }
        LockGuard(const LockGuard&)            = delete;
        LockGuard& operator=(const LockGuard&) = delete;
    private:
        k_mutex& mM;
    };

    chip::EndpointId   mEndpoint{1};
    bool               mInitialised{false};
    mutable k_mutex    mLock;
    CommandPumpHandler mPumpHandler{nullptr};
    std::vector<ChangedAttributesListener*> mListeners; // reserved in Init()

    // std::optional defers construction to Init() because the production
    // pieces (Reconciler, AtomicTxn) hold references that need to bind to
    // long-lived storage. Order of declaration is also their construction
    // order; tear down in reverse via Shutdown().
    std::optional<LogicalACState>      mState;
    std::optional<MonotonicTimeSource> mTime;
    std::optional<Reconciler>          mReconciler;
    std::optional<AtomicTxn>           mAtomic;
    // mReader holds references into mState and mReconciler. Declared last
    // so destruction order tears it down before its referents; Shutdown()
    // enforces the same order explicitly.
    std::optional<SyncReader>          mReader;
};

} // namespace sync
