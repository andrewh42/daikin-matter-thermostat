/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * SyncReader — thread-safe per-attribute projected reads for the AAI Read
 * paths (and debug surfaces). Holds references into state owned by
 * SyncStack; must not outlive its owner.
 *
 * Each method acquires SyncStack's mutex, projects via the Reconciler's
 * Projector, and returns the value. Aggregated reads (multiple fields in
 * one client request) cost N lock cycles, which is fine — the CHIP stack
 * lock above is held by the AAI caller anyway, so the only contention is
 * with the S21 work queue.
 */
#pragma once

#include "logical_ac_state.h"
#include "reconciler.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app/data-model/Nullable.h>

#include <cstdint>
#include <zephyr/kernel.h>

namespace sync {

class SyncReader {
public:
    SyncReader(k_mutex& lock, const LogicalACState& state, const Reconciler& reconciler)
        : mLock(lock), mState(state), mReconciler(reconciler) {}

    SyncReader(const SyncReader&)            = delete;
    SyncReader& operator=(const SyncReader&) = delete;

    bool                                     ReadOnOff()                       const;
    SystemModeEnum                           ReadSystemMode()                  const;
    int16_t                                  ReadOccupiedHeatingSetpoint()     const;
    int16_t                                  ReadOccupiedCoolingSetpoint()     const;
    ThermostatRunningModeEnum                ReadRunningMode()                 const;
    chip::app::DataModel::Nullable<int16_t>  ReadLocalTemperature()            const;
    chip::app::DataModel::Nullable<int16_t>  ReadOutdoorTemperature()          const;
    SetpointChangeSourceEnum                 ReadSetpointChangeSource()        const;
    chip::app::DataModel::Nullable<uint8_t>  ReadSpeedSetting()                const;
    FanModeEnum                              ReadFanMode()                     const;
    uint8_t                                  ReadSpeedCurrent()                const;
    chip::app::DataModel::Nullable<uint16_t> ReadHumidityCentiPercent()        const;
    bool                                     ReadReachable()                   const;

private:
    k_mutex&              mLock;
    const LogicalACState& mState;
    const Reconciler&     mReconciler;
};

} // namespace sync
