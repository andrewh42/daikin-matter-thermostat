/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#pragma once

#include "bridge_kernel.h"
#include "change_publisher.h"
#include "changed_attributes_listener.h"
#include "logical_ac_state.h"
#include "logical_attribute.h"
#include "monotonic_time_source.h"
#include "operational_mode.h"
#include "twin_field.h"
#include "write_intent.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <lib/core/CHIPError.h>

#include <cstdint>
#include <optional>
#include <vector>
#include <zephyr/kernel.h>

namespace sync {

/**
 * SyncCoordinator is the production singleton wrapping the bridge kernel
 * with a Zephyr mutex and a ChangePublisher.
 *
 * Composition:
 *   - BridgeKernel: data ownership and policy (LogicalACState,
 *     Reconciler). Lock-free; this class adds the serialisation.
 *   - ChangePublisher: listener registry + dispatch.
 *   - AAIInstaller: CHIP AAI registry plumbing (owned outside
 *     SyncCoordinator; installed/uninstalled in Init/Shutdown).
 *
 * Thread model: one internal mutex serialises every public mutation and
 * every per-attribute Read. Listener callbacks fire outside the mutex so
 * they can grab the CHIP stack lock or schedule onto the Matter event
 * loop without deadlocking. A per-attribute Read takes the lock once;
 * callers that need several projected fields coherently should prefer
 * `ProjectionSnapshot()`, which takes the lock exactly once for the whole
 * projection rather than once per field.
 */
class SyncCoordinator {
public:
    static constexpr size_t kMaxListeners = ChangePublisher::kMaxListeners;

    static SyncCoordinator& Instance();

    CHIP_ERROR Init(chip::EndpointId endpoint = 1);
    void       Shutdown();

    // ─── Mutating entry points (lock-acquiring) ──────────────────────────────

    OperationalChange   ApplyIntent(const WriteIntent& intent);
    OperationalChange   ApplyOperationalObservation(const S21OperationalObservation& obs);
    EnvironmentalChange ApplyEnvironmentalObservation(const S21EnvironmentalObservation& obs);
    void                OnCommandSent(const S21OperationCommand& cmd);
    void                OnCommandFailed();
    void                NotifyLinkDown();
    std::optional<S21OperationCommand> PendingCommand() const;

    using CommandPumpHandler = ChangePublisher::CommandPumpHandler;
    void SetCommandPumpHandler(CommandPumpHandler handler) { mPublisher.SetCommandPumpHandler(handler); }

    CHIP_ERROR AddChangedAttributesListener(ChangedAttributesListener* listener);
    void       RemoveChangedAttributesListener(ChangedAttributesListener* listener);

    // ─── Per-attribute projected reads (lock-acquiring) ──────────────────────

    bool                    ReadOnOff()                       const;
    OperationalMode         ReadMode()                        const;
    int16_t                 ReadOccupiedHeatingSetpoint()     const;
    int16_t                 ReadOccupiedCoolingSetpoint()     const;
    RunningMode             ReadRunningMode()                 const;
    std::optional<int16_t>  ReadLocalTemperature()            const;
    std::optional<int16_t>  ReadOutdoorTemperature()          const;
    ObservationSource       ReadSetpointSource()              const;
    std::optional<uint8_t>  ReadSpeedSetting()                const;
    FanModeCategory         ReadFanMode()                     const;
    uint8_t                 ReadSpeedCurrent()                const;
    std::optional<uint8_t>  ReadPercentSetting()              const;
    uint8_t                 ReadPercentCurrent()              const;
    std::optional<uint16_t> ReadHumidityCentiPercent()        const;
    bool                    ReadReachable()                   const;

    /// Coherent multi-field read: one lock acquisition yields a frozen
    /// ProjectedClusterState whose fields are all drawn from the same
    /// LogicalACState. Use this from any AAI Read that needs more than one
    /// projected value at once (the Thermostat SystemMode = f(onOff, mode)
    /// case being canonical); the per-attribute Read* methods stay the
    /// right surface for single-field callers.
    ///
    /// Callers must not already hold the internal lock.
    ProjectedClusterState ProjectionSnapshot() const;

    /// Value-copy of the underlying state, taken under the internal lock.
    /// For debug surfaces (shell commands, logs) that want the raw
    /// observed/desired/inFlight triples rather than projected reads.
    LogicalACState Snapshot() const;

    chip::EndpointId Endpoint() const { return mEndpoint; }

private:
    SyncCoordinator() = default;
    ~SyncCoordinator() = default;
    SyncCoordinator(const SyncCoordinator&)            = delete;
    SyncCoordinator& operator=(const SyncCoordinator&) = delete;

    class LockGuard {
    public:
        explicit LockGuard(k_mutex& m) : mM(m) { k_mutex_lock(&m, K_FOREVER); }
        ~LockGuard()                            { k_mutex_unlock(&mM); }
        LockGuard(const LockGuard&)            = delete;
        LockGuard& operator=(const LockGuard&) = delete;
    private:
        k_mutex& mM;
    };

    chip::EndpointId            mEndpoint{1};
    bool                        mInitialised{false};
    mutable k_mutex             mLock;
    ChangePublisher             mPublisher;
    MonotonicTimeSource         mTime;
    std::optional<BridgeKernel> mKernel;
};

} // namespace sync
