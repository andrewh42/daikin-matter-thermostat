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
#include "logical_ac_state.h"
#include "matter_attribute_path.h"
#include "monotonic_time_source.h"
#include "projector.h"
#include "reconciler.h"
#include "write_intent.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app/data-model/Nullable.h>
#include <lib/core/CHIPError.h>

#include <optional>
#include <vector>
#include <zephyr/kernel.h>

namespace sync {

class SyncStack {
public:
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
    AppliedChange ApplyIntent(const WriteIntent& intent);

    /// Apply a poll observation. Called from the S21 work queue. Returns
    /// the dirty-path set; the caller posts each path to the Matter event
    /// loop for MatterReportingAttributeChangeCallback under LockChipStack.
    AppliedChange ApplyObservation(const S21State& obs);

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

    /// Hook called from inside ApplyIntent and ApplyObservation when those
    /// produce a pendingCommand. The Phase 7 pump uses it to wake itself
    /// without us needing to know about Zephyr work queues here.
    using CommandPumpHook = void (*)();
    void SetCommandPumpHook(CommandPumpHook hook) { mPumpHook = hook; }

    /// Hook called from inside ApplyIntent and ApplyObservation when the
    /// dirty-attribute set is non-empty. The pump posts each path to
    /// MatterReportingAttributeChangeCallback under the CHIP stack lock.
    /// Kept separate from CommandPumpHook so the two consumers run on
    /// different threads (S21 work queue vs. Matter event loop).
    using DirtyReporterHook = void (*)(const std::vector<MatterAttributePath>&);
    void SetDirtyReporterHook(DirtyReporterHook hook) { mDirtyHook = hook; }

    // ─── Atomic-request transaction (Matter AtomicRequest command) ───────────

    AtomicTxn::Status BeginAtomic();
    AtomicTxn::Status AtomicWrite(const WriteIntent& intent);
    AppliedChange     CommitAtomic();
    AtomicTxn::Status RollbackAtomic();

    // ─── Per-attribute projected reads (lock-acquiring) ──────────────────────
    //
    // The AAI Read paths call these to compose attribute encoder output.
    // Each grabs the mutex internally; aggregated reads (e.g. multiple
    // fields in one client request) cost N lock cycles, which is fine —
    // the CHIP stack lock above is held by the caller anyway, so the only
    // contention is with the S21 work queue.

    bool                          ReadOnOff()                       const;
    SystemModeEnum                ReadSystemMode()                  const;
    int16_t                       ReadOccupiedHeatingSetpoint()     const;
    int16_t                       ReadOccupiedCoolingSetpoint()     const;
    ThermostatRunningModeEnum     ReadRunningMode()                 const;
    chip::app::DataModel::Nullable<int16_t> ReadLocalTemperature()  const;
    chip::app::DataModel::Nullable<int16_t> ReadOutdoorTemperature() const;
    SetpointChangeSourceEnum      ReadSetpointChangeSource()        const;
    chip::app::DataModel::Nullable<uint8_t> ReadSpeedSetting()      const;
    FanModeEnum                   ReadFanMode()                     const;
    uint8_t                       ReadSpeedCurrent()                const;
    uint16_t                      ReadHumidityCentiPercent()        const;
    bool                          ReadReachable()                   const;

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

    chip::EndpointId  mEndpoint{1};
    bool              mInitialised{false};
    mutable k_mutex   mLock;
    CommandPumpHook   mPumpHook{nullptr};
    DirtyReporterHook mDirtyHook{nullptr};

    // std::optional defers construction to Init() because the production
    // pieces (Reconciler, AtomicTxn) hold references that need to bind to
    // long-lived storage. Order of declaration is also their construction
    // order; tear down in reverse via Shutdown().
    std::optional<LogicalACState>      mState;
    std::optional<MonotonicTimeSource> mTime;
    std::optional<Reconciler>          mReconciler;
    std::optional<AtomicTxn>           mAtomic;
};

} // namespace sync
