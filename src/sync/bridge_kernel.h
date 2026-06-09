/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * BridgeKernel
 * ------------
 * The data-ownership root of the bridge model. Owns:
 *
 *   - LogicalACState   — the canonical air-conditioner state
 *   - MonotonicTimeSource — the time-source the reconciler reads
 *   - Reconciler       — twin policy + diff computation
 *   - AtomicTxn        — Matter AtomicRequest support
 *
 * Mutating methods (`applyIntent`, `applyOperationalObservation`, …)
 * return change records but do **not** lock. The caller (today: SyncCoordinator)
 * is responsible for serialising access. Per-attribute reads are similarly
 * lock-free: caller holds whatever lock is needed.
 *
 * No Matter or Zephyr dependencies. CHIP-free.
 */
#pragma once

#include "atomic_buffer.h"
#include "logical_ac_state.h"
#include "logical_attribute.h"
#include "operational_mode.h"
#include "projector.h"
#include "reconciler.h"
#include "s21_command.h"
#include "s21_observation.h"
#include "time_source.h"
#include "twin_field.h"
#include "write_intent.h"

#include <cstdint>
#include <optional>

namespace sync {

class BridgeKernel {
public:
    /// TimeSource is caller-owned (host tests inject ManualTimeSource;
    /// production wraps MonotonicTimeSource). Letting BridgeKernel own
    /// the time source would force a Zephyr include and break host tests.
    explicit BridgeKernel(TimeSource& time, const ReconcilerConfig& cfg = {})
        : mState(), mReconciler(mState, time, cfg), mAtomic(mReconciler, time)
    {
    }

    BridgeKernel(const BridgeKernel&)            = delete;
    BridgeKernel& operator=(const BridgeKernel&) = delete;

    // ─── Mutation ────────────────────────────────────────────────────────────

    OperationalChange   applyIntent(const WriteIntent& intent)            { return mReconciler.applyIntent(intent); }
    OperationalChange   applyOperationalObservation(const S21OperationalObservation& obs)
                                                                          { return mReconciler.applyOperationalObservation(obs); }
    EnvironmentalChange applyEnvironmentalObservation(const S21EnvironmentalObservation& obs)
                                                                          { return mReconciler.applyEnvironmentalObservation(obs); }
    void                onCommandSent(const S21OperationCommand& cmd)     { mReconciler.onCommandSent(cmd); }
    void                onCommandFailed()                                 { mReconciler.onCommandFailed(); }
    void                notifyLinkDown()                                  { mState.reachable.applyObservation(false); }
    std::optional<S21OperationCommand> pendingCommand() const             { return mReconciler.pendingCommand(); }

    // ─── Atomic ──────────────────────────────────────────────────────────────

    AtomicTxn::Status begin()                                  { return mAtomic.begin(); }
    AtomicTxn::Status atomicWrite(const WriteIntent& intent)   { return mAtomic.write(intent); }
    OperationalChange commit()                                 { return mAtomic.commit(); }
    AtomicTxn::Status rollback()                               { return mAtomic.rollback(); }

    // ─── Snapshot for debug surfaces ─────────────────────────────────────────

    LogicalACState snapshot() const { return mState; }

    // ─── Per-attribute reads (lock-free; caller holds external lock) ─────────

    bool                    readOnOff()                       const { return projector().projectedOnOff(mState); }
    OperationalMode         readMode()                        const { return projector().projectedMode(mState); }
    int16_t                 readOccupiedHeatingSetpoint()     const { return projector().projectedOccupiedHeatingSetpoint(mState); }
    int16_t                 readOccupiedCoolingSetpoint()     const { return projector().projectedOccupiedCoolingSetpoint(mState); }
    RunningMode             readRunningMode()                 const { return projector().projectedRunningMode(mState); }
    std::optional<int16_t>  readLocalTemperature()            const { return projector().projectedLocalTemperature(mState); }
    std::optional<int16_t>  readOutdoorTemperature()          const { return projector().projectedOutdoorTemperature(mState); }
    ObservationSource       readSetpointSource()              const { return projector().projectedSetpointSource(mState); }
    FanSpeed                readSpeedSetting()                const { return projector().projectedSpeedSetting(mState); }
    bool                    readFanIsAuto()                   const { return projector().projectedFanIsAuto(mState); }
    uint8_t                 readSpeedCurrent()                const { return projector().projectedSpeedCurrent(mState); }
    std::optional<uint16_t> readHumidityCentiPercent()        const { return projector().projectedHumidityCentiPercent(mState); }
    bool                    readReachable()                   const { return projector().projectedReachable(mState); }

    const Projector& projector() const { return mReconciler.projector(); }

private:
    LogicalACState mState;
    Reconciler     mReconciler;
    AtomicTxn      mAtomic;
};

} // namespace sync
