/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#include "sync_stack.h"

#include "aai_thermostat.h"
#include "aai_onoff.h"
#include "aai_fan_control.h"
#include "aai_humidity.h"

#include <app/AttributeAccessInterfaceRegistry.h>
#include <app/clusters/thermostat-server/thermostat-server.h>
#include <zephyr/logging/log.h>

#include <algorithm>

LOG_MODULE_REGISTER(sync_stack, LOG_LEVEL_DBG);

namespace sync {

namespace {

// AAI instances are file-local statics, not members of SyncStack, because
// they need to outlive any test-time construction/destruction of the
// singleton (the AAI registry holds raw pointers and would dangle).
sync_aai::OnOffBridgeAttributeAccess       gOnOffAai;
sync_aai::ThermostatBridgeAttributeAccess  gThermostatAai;
sync_aai::FanControlBridgeAttributeAccess  gFanControlAai;
sync_aai::HumidityBridgeAttributeAccess    gHumidityAai;

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
    ok = AttributeAccessInterfaceRegistry::Instance().Register(&gHumidityAai)   && ok;
    return ok;
}

void unregisterAAIs()
{
    using namespace chip::app;
    AttributeAccessInterfaceRegistry::Instance().Unregister(&gThermostatAai);
    AttributeAccessInterfaceRegistry::Instance().Unregister(&gOnOffAai);
    AttributeAccessInterfaceRegistry::Instance().Unregister(&gFanControlAai);
    AttributeAccessInterfaceRegistry::Instance().Unregister(&gHumidityAai);
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
    mListeners.reserve(kMaxListeners); // size cap is contract; reserve avoids realloc

    mState.emplace();                                 // boot defaults
    mTime.emplace();
    mReconciler.emplace(*mState, *mTime,
                        ReconcilerConfig{.endpoint = endpoint});
    mAtomic.emplace(*mReconciler, *mTime);
    mReader.emplace(mLock, *mState, *mReconciler);

    // Inject our state pointers into the AAI singletons. They're file-
    // local statics in this TU; their lifetime exceeds SyncStack's.
    gOnOffAai.Bind(this);
    gThermostatAai.Bind(this);
    gFanControlAai.Bind(this);
    gHumidityAai.Bind(this);

    if (!registerAAIs(endpoint)) {
        LOG_ERR("Failed to register one or more bridge AAIs");
        // Best-effort cleanup; we may have registered some.
        unregisterAAIs();
        mReader.reset();
        mAtomic.reset();
        mReconciler.reset();
        mTime.reset();
        mState.reset();
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
    // Tear down in reverse construction order: mReader holds references
    // into mState and mReconciler, so it must die first.
    mReader.reset();
    mAtomic.reset();
    mReconciler.reset();
    mTime.reset();
    mState.reset();
    mListeners.clear();
    mPumpHandler = nullptr;
    mInitialised = false;
}

// ─── Change-listener registration ────────────────────────────────────────────

CHIP_ERROR SyncStack::AddChangedAttributesListener(ChangedAttributesListener* listener)
{
    if (listener == nullptr) return CHIP_ERROR_INVALID_ARGUMENT;
    LockGuard g(mLock);
    if (std::find(mListeners.begin(), mListeners.end(), listener) != mListeners.end()) {
        return CHIP_NO_ERROR; // idempotent
    }
    if (mListeners.size() >= kMaxListeners) return CHIP_ERROR_NO_MEMORY;
    mListeners.push_back(listener); // capacity was reserved in Init(), no realloc
    return CHIP_NO_ERROR;
}

void SyncStack::RemoveChangedAttributesListener(ChangedAttributesListener* listener)
{
    if (listener == nullptr) return;
    LockGuard g(mLock);
    mListeners.erase(
        std::remove(mListeners.begin(), mListeners.end(), listener),
        mListeners.end());
}

// ─── Mutating entry points ────────────────────────────────────────────────────

namespace {

// Snapshot the listener vector under the lock so the dispatch loop runs
// outside the lock with a stable view, even if Add/Remove is racing on
// another thread. Stack-allocated; capped at kMaxListeners.
struct ListenerSnapshot {
    ChangedAttributesListener* slots[SyncStack::kMaxListeners];
    size_t count{0};

    void notify(const std::vector<MatterAttributePath>& paths) const
    {
        for (size_t i = 0; i < count; ++i) slots[i]->OnChangedAttributes(paths);
    }
};

} // namespace

OperationalChange SyncStack::ApplyIntent(const WriteIntent& intent)
{
    OperationalChange change;
    ListenerSnapshot  snap;
    {
        LockGuard g(mLock);
        change = mReconciler->applyIntent(intent);
        snap.count = mListeners.size();
        std::copy(mListeners.begin(), mListeners.end(), snap.slots);
    }
    // Notifications fire outside the lock — listeners may schedule work
    // or grab the CHIP stack lock, both of which would deadlock if held
    // under mLock.
    if (!change.dirtyAttributes.empty()) snap.notify(change.dirtyAttributes);
    if (mPumpHandler && change.sendCommand.has_value()) mPumpHandler();
    return change;
}

OperationalChange SyncStack::ApplyOperationalObservation(
    const S21OperationalObservation& obs)
{
    OperationalChange change;
    ListenerSnapshot  snap;
    {
        LockGuard g(mLock);
        change = mReconciler->applyOperationalObservation(obs);
        snap.count = mListeners.size();
        std::copy(mListeners.begin(), mListeners.end(), snap.slots);
    }
    if (!change.dirtyAttributes.empty()) snap.notify(change.dirtyAttributes);
    if (mPumpHandler && change.sendCommand.has_value()) mPumpHandler();
    return change;
}

EnvironmentalChange SyncStack::ApplyEnvironmentalObservation(
    const S21EnvironmentalObservation& obs)
{
    EnvironmentalChange change;
    ListenerSnapshot    snap;
    {
        LockGuard g(mLock);
        change = mReconciler->applyEnvironmentalObservation(obs);
        snap.count = mListeners.size();
        std::copy(mListeners.begin(), mListeners.end(), snap.slots);
    }
    if (!change.dirtyAttributes.empty()) snap.notify(change.dirtyAttributes);
    // No pump check: env observations touch only SensorFields and cannot
    // produce a sendCommand. The narrower return type makes this static.
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
    mState->reachable.applyObservation(false);
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

OperationalChange SyncStack::CommitAtomic()
{
    LockGuard g(mLock);
    return mAtomic->commit();
}

AtomicTxn::Status SyncStack::RollbackAtomic()
{
    LockGuard g(mLock);
    return mAtomic->rollback();
}

// ─── Raw state snapshot (debug surfaces) ─────────────────────────────────────

LogicalACState SyncStack::Snapshot() const
{
    LockGuard g(mLock);
    return *mState;
}

} // namespace sync
