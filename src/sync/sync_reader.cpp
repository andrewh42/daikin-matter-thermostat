/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#include "sync_reader.h"

#include "projector.h"

namespace sync {

namespace {

// Reduce repetition: each Read* is "lock; project; return".
template <typename Fn>
auto projectedRead(k_mutex& m,
                   const LogicalACState& state,
                   const Reconciler& rec, Fn&& fn)
    -> decltype(fn(rec.projector(), state))
{
    k_mutex_lock(&m, K_FOREVER);
    auto v = fn(rec.projector(), state);
    k_mutex_unlock(&m);
    return v;
}

} // namespace

bool SyncReader::ReadOnOff() const
{
    return projectedRead(mLock, mState, mReconciler,
        [](const Projector& p, const LogicalACState& s) { return p.projectedOnOff(s); });
}

SystemModeEnum SyncReader::ReadSystemMode() const
{
    return projectedRead(mLock, mState, mReconciler,
        [](const Projector& p, const LogicalACState& s) { return p.projectedSystemMode(s); });
}

int16_t SyncReader::ReadOccupiedHeatingSetpoint() const
{
    return projectedRead(mLock, mState, mReconciler,
        [](const Projector& p, const LogicalACState& s) {
            return p.projectedOccupiedHeatingSetpoint(s);
        });
}

int16_t SyncReader::ReadOccupiedCoolingSetpoint() const
{
    return projectedRead(mLock, mState, mReconciler,
        [](const Projector& p, const LogicalACState& s) {
            return p.projectedOccupiedCoolingSetpoint(s);
        });
}

ThermostatRunningModeEnum SyncReader::ReadRunningMode() const
{
    return projectedRead(mLock, mState, mReconciler,
        [](const Projector& p, const LogicalACState& s) { return p.projectedRunningMode(s); });
}

chip::app::DataModel::Nullable<int16_t> SyncReader::ReadLocalTemperature() const
{
    auto v = projectedRead(mLock, mState, mReconciler,
        [](const Projector& p, const LogicalACState& s) { return p.projectedLocalTemperature(s); });
    chip::app::DataModel::Nullable<int16_t> out;
    if (v.has_value()) out.SetNonNull(*v); else out.SetNull();
    return out;
}

chip::app::DataModel::Nullable<int16_t> SyncReader::ReadOutdoorTemperature() const
{
    auto v = projectedRead(mLock, mState, mReconciler,
        [](const Projector& p, const LogicalACState& s) { return p.projectedOutdoorTemperature(s); });
    chip::app::DataModel::Nullable<int16_t> out;
    if (v.has_value()) out.SetNonNull(*v); else out.SetNull();
    return out;
}

SetpointChangeSourceEnum SyncReader::ReadSetpointChangeSource() const
{
    return projectedRead(mLock, mState, mReconciler,
        [](const Projector& p, const LogicalACState& s) { return p.projectedSetpointSource(s); });
}

chip::app::DataModel::Nullable<uint8_t> SyncReader::ReadSpeedSetting() const
{
    auto v = projectedRead(mLock, mState, mReconciler,
        [](const Projector& p, const LogicalACState& s) { return p.projectedSpeedSetting(s); });
    chip::app::DataModel::Nullable<uint8_t> out;
    // FanLevel's underlying values match the cluster's SpeedSetting wire
    // format, so this cast is the round-trip of the AAI Write decode.
    if (v.has_value()) out.SetNonNull(static_cast<uint8_t>(*v));
    else               out.SetNull();
    return out;
}

FanModeEnum SyncReader::ReadFanMode() const
{
    return projectedRead(mLock, mState, mReconciler,
        [](const Projector& p, const LogicalACState& s) { return p.projectedFanMode(s); });
}

uint8_t SyncReader::ReadSpeedCurrent() const
{
    return projectedRead(mLock, mState, mReconciler,
        [](const Projector& p, const LogicalACState& s) { return p.projectedSpeedCurrent(s); });
}

chip::app::DataModel::Nullable<uint16_t> SyncReader::ReadHumidityCentiPercent() const
{
    auto v = projectedRead(mLock, mState, mReconciler,
        [](const Projector& p, const LogicalACState& s) {
            return p.projectedHumidityCentiPercent(s);
        });
    chip::app::DataModel::Nullable<uint16_t> out;
    if (v.has_value()) out.SetNonNull(*v); else out.SetNull();
    return out;
}

bool SyncReader::ReadReachable() const
{
    return projectedRead(mLock, mState, mReconciler,
        [](const Projector& p, const LogicalACState& s) { return p.projectedReachable(s); });
}

} // namespace sync
