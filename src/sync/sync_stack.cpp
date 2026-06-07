/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#include "sync_stack.h"

#include "aai_thermostat.h"
#include "aai_onoff.h"
#include "aai_fan_control.h"

#include <app/AttributeAccessInterfaceRegistry.h>
#include <app/clusters/thermostat-server/thermostat-server.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sync_stack, LOG_LEVEL_DBG);

namespace sync {

namespace {

// AAI instances are file-local statics, not members of SyncStack, because
// they need to outlive any test-time construction/destruction of the
// singleton (the AAI registry holds raw pointers and would dangle).
sync_aai::OnOffBridgeAttributeAccess       gOnOffAai;
sync_aai::ThermostatBridgeAttributeAccess  gThermostatAai;
sync_aai::FanControlBridgeAttributeAccess  gFanControlAai;

bool registerAAIs(chip::EndpointId endpoint)
{
    using namespace chip::app;

    // Replace the SDK's wildcard ThermostatAttrAccess. It's a file-local
    // static inside thermostat-server.cpp (line 80), so we can't reach it
    // by name from here — but Get() finds it by (endpoint, cluster) match
    // because the registration uses Optional<EndpointId>::Missing().
    // See sync-option-4-plan Phase 5 "Why not subclass".
    auto* existing = AttributeAccessInterfaceRegistry::Instance().Get(
        endpoint, Clusters::Thermostat::Id);
    if (existing != nullptr) {
        AttributeAccessInterfaceRegistry::Instance().Unregister(existing);
    }

    bool ok = true;
    ok = AttributeAccessInterfaceRegistry::Instance().Register(&gThermostatAai) && ok;
    ok = AttributeAccessInterfaceRegistry::Instance().Register(&gOnOffAai)      && ok;
    ok = AttributeAccessInterfaceRegistry::Instance().Register(&gFanControlAai) && ok;
    return ok;
}

void unregisterAAIs()
{
    using namespace chip::app;
    AttributeAccessInterfaceRegistry::Instance().Unregister(&gThermostatAai);
    AttributeAccessInterfaceRegistry::Instance().Unregister(&gOnOffAai);
    AttributeAccessInterfaceRegistry::Instance().Unregister(&gFanControlAai);
}

} // namespace

SyncStack& SyncStack::Instance()
{
    static SyncStack sInstance;
    return sInstance;
}

CHIP_ERROR SyncStack::Init(chip::EndpointId endpoint)
{
    if (mInitialised) return CHIP_NO_ERROR;

    mEndpoint = endpoint;
    k_mutex_init(&mLock);

    mState.emplace();                                 // boot defaults
    mTime.emplace();
    mReconciler.emplace(*mState, *mTime,
                        ReconcilerConfig{.endpoint = endpoint});
    mAtomic.emplace(*mReconciler, *mTime);

    // Inject our state pointers into the AAI singletons. They're file-
    // local statics in this TU; their lifetime exceeds SyncStack's.
    gOnOffAai.Bind(this);
    gThermostatAai.Bind(this);
    gFanControlAai.Bind(this);

    if (!registerAAIs(endpoint)) {
        LOG_ERR("Failed to register one or more bridge AAIs");
        // Best-effort cleanup; we may have registered some.
        unregisterAAIs();
        mState.reset();
        mTime.reset();
        mReconciler.reset();
        mAtomic.reset();
        return CHIP_ERROR_INTERNAL;
    }

    mInitialised = true;
    LOG_INF("SyncStack initialised on endpoint %u", endpoint);
    return CHIP_NO_ERROR;
}

void SyncStack::Shutdown()
{
    if (!mInitialised) return;
    unregisterAAIs();
    mAtomic.reset();
    mReconciler.reset();
    mTime.reset();
    mState.reset();
    mInitialised = false;
}

// ─── Mutating entry points ────────────────────────────────────────────────────

AppliedChange SyncStack::ApplyIntent(const WriteIntent& intent)
{
    AppliedChange change;
    {
        LockGuard g(mLock);
        change = mReconciler->applyIntent(intent);
    }
    // Hooks fire outside the lock — they may schedule work or grab the
    // CHIP stack lock, both of which would deadlock if held under mLock.
    if (mDirtyHook && !change.dirtyAttributes.empty()) mDirtyHook(change.dirtyAttributes);
    if (mPumpHook  && change.sendCommand.has_value()) mPumpHook();
    return change;
}

AppliedChange SyncStack::ApplyObservation(const S21State& obs)
{
    AppliedChange change;
    {
        LockGuard g(mLock);
        change = mReconciler->applyObservation(obs);
    }
    if (mDirtyHook && !change.dirtyAttributes.empty()) mDirtyHook(change.dirtyAttributes);
    if (mPumpHook  && change.sendCommand.has_value()) mPumpHook();
    return change;
}

void SyncStack::OnCommandSent(const S21OperationCommand& cmd)
{
    LockGuard g(mLock);
    mReconciler->onCommandSent(cmd);
}

void SyncStack::OnCommandFailed()
{
    LockGuard g(mLock);
    mReconciler->onCommandFailed();
}

void SyncStack::NotifyLinkDown()
{
    LockGuard g(mLock);
    // Marking Device source so the reachable twin doesn't claim a fake
    // Matter-source observation; this also resets the guard window.
    mState->reachable.applyObservation(false, ObservationSource::Device);
}

std::optional<S21OperationCommand> SyncStack::PendingCommand() const
{
    LockGuard g(mLock);
    return mReconciler->pendingCommand();
}

// ─── Atomic-request transaction ──────────────────────────────────────────────

AtomicTxn::Status SyncStack::BeginAtomic()
{
    LockGuard g(mLock);
    return mAtomic->begin();
}

AtomicTxn::Status SyncStack::AtomicWrite(const WriteIntent& intent)
{
    LockGuard g(mLock);
    return mAtomic->write(intent);
}

AppliedChange SyncStack::CommitAtomic()
{
    LockGuard g(mLock);
    return mAtomic->commit();
}

AtomicTxn::Status SyncStack::RollbackAtomic()
{
    LockGuard g(mLock);
    return mAtomic->rollback();
}

// ─── Per-attribute projected reads ───────────────────────────────────────────

namespace {

// Reduce repetition: each Read* is "lock; project; return".
template <typename Fn>
auto projectedRead(k_mutex& m,
                   const LogicalACState* state,
                   const Reconciler* rec, Fn&& fn)
    -> decltype(fn(rec->projector(), *state))
{
    k_mutex_lock(&m, K_FOREVER);
    auto v = fn(rec->projector(), *state);
    k_mutex_unlock(&m);
    return v;
}

} // namespace

bool SyncStack::ReadOnOff() const
{
    return projectedRead(mLock, &*mState, &*mReconciler,
        [](const Projector& p, const LogicalACState& s) { return p.projectedOnOff(s); });
}

SystemModeEnum SyncStack::ReadSystemMode() const
{
    return projectedRead(mLock, &*mState, &*mReconciler,
        [](const Projector& p, const LogicalACState& s) { return p.projectedSystemMode(s); });
}

int16_t SyncStack::ReadOccupiedHeatingSetpoint() const
{
    return projectedRead(mLock, &*mState, &*mReconciler,
        [](const Projector& p, const LogicalACState& s) {
            return p.projectedOccupiedHeatingSetpoint(s);
        });
}

int16_t SyncStack::ReadOccupiedCoolingSetpoint() const
{
    return projectedRead(mLock, &*mState, &*mReconciler,
        [](const Projector& p, const LogicalACState& s) {
            return p.projectedOccupiedCoolingSetpoint(s);
        });
}

ThermostatRunningModeEnum SyncStack::ReadRunningMode() const
{
    return projectedRead(mLock, &*mState, &*mReconciler,
        [](const Projector& p, const LogicalACState& s) { return p.projectedRunningMode(s); });
}

chip::app::DataModel::Nullable<int16_t> SyncStack::ReadLocalTemperature() const
{
    auto v = projectedRead(mLock, &*mState, &*mReconciler,
        [](const Projector& p, const LogicalACState& s) { return p.projectedLocalTemperature(s); });
    chip::app::DataModel::Nullable<int16_t> out;
    if (v.has_value()) out.SetNonNull(*v); else out.SetNull();
    return out;
}

chip::app::DataModel::Nullable<int16_t> SyncStack::ReadOutdoorTemperature() const
{
    auto v = projectedRead(mLock, &*mState, &*mReconciler,
        [](const Projector& p, const LogicalACState& s) { return p.projectedOutdoorTemperature(s); });
    chip::app::DataModel::Nullable<int16_t> out;
    if (v.has_value()) out.SetNonNull(*v); else out.SetNull();
    return out;
}

SetpointChangeSourceEnum SyncStack::ReadSetpointChangeSource() const
{
    return projectedRead(mLock, &*mState, &*mReconciler,
        [](const Projector& p, const LogicalACState& s) { return p.projectedSetpointSource(s); });
}

chip::app::DataModel::Nullable<uint8_t> SyncStack::ReadSpeedSetting() const
{
    auto v = projectedRead(mLock, &*mState, &*mReconciler,
        [](const Projector& p, const LogicalACState& s) { return p.projectedSpeedSetting(s); });
    chip::app::DataModel::Nullable<uint8_t> out;
    // FanLevel's underlying values match the cluster's SpeedSetting wire
    // format, so this cast is the round-trip of the AAI Write decode.
    if (v.has_value()) out.SetNonNull(static_cast<uint8_t>(*v));
    else               out.SetNull();
    return out;
}

FanModeEnum SyncStack::ReadFanMode() const
{
    return projectedRead(mLock, &*mState, &*mReconciler,
        [](const Projector& p, const LogicalACState& s) { return p.projectedFanMode(s); });
}

uint8_t SyncStack::ReadSpeedCurrent() const
{
    return projectedRead(mLock, &*mState, &*mReconciler,
        [](const Projector& p, const LogicalACState& s) { return p.projectedSpeedCurrent(s); });
}

uint16_t SyncStack::ReadHumidityCentiPercent() const
{
    return projectedRead(mLock, &*mState, &*mReconciler,
        [](const Projector& p, const LogicalACState& s) {
            return p.projectedHumidityCentiPercent(s);
        });
}

bool SyncStack::ReadReachable() const
{
    return projectedRead(mLock, &*mState, &*mReconciler,
        [](const Projector& p, const LogicalACState& s) { return p.projectedReachable(s); });
}

} // namespace sync
