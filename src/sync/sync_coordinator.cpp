/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#include "sync_coordinator.h"

#include "aai_installer.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sync_coordinator, LOG_LEVEL_DBG);

namespace sync {

namespace {

// AAI instances live inside AAIInstaller, which is a singleton-scoped
// static so the AAIs outlive any test-time construction/destruction of
// SyncCoordinator (the AAI registry holds raw pointers and would dangle).
AAIInstaller& installer()
{
    static AAIInstaller sInstaller;
    return sInstaller;
}

} // namespace

SyncCoordinator& SyncCoordinator::Instance()
{
    static SyncCoordinator sInstance;
    return sInstance;
}

CHIP_ERROR SyncCoordinator::Init(chip::EndpointId endpoint)
{
    if (mInitialised) return CHIP_NO_ERROR;

    mEndpoint = endpoint;
    k_mutex_init(&mLock);

    mKernel.emplace(mTime, ReconcilerConfig{});

    if (installer().Install(endpoint, this) != CHIP_NO_ERROR) {
        LOG_ERR("Failed to install bridge AAIs");
        mKernel.reset();
        return CHIP_ERROR_INTERNAL;
    }

    mInitialised = true;
    LOG_INF("SyncCoordinator initialised on endpoint %u", endpoint);
    return CHIP_NO_ERROR;
}

void SyncCoordinator::Shutdown()
{
    if (!mInitialised) return;
    installer().Uninstall();
    mKernel.reset();
    mPublisher.Reset();
    mInitialised = false;
}

// ─── Change-listener registration ────────────────────────────────────────────

CHIP_ERROR SyncCoordinator::AddChangedAttributesListener(ChangedAttributesListener* listener)
{
    LockGuard g(mLock);
    switch (mPublisher.Add(listener)) {
    case ChangePublisher::Status::Ok:
    case ChangePublisher::Status::AlreadyRegistered: return CHIP_NO_ERROR;
    case ChangePublisher::Status::Full:              return CHIP_ERROR_NO_MEMORY;
    case ChangePublisher::Status::InvalidArgument:   return CHIP_ERROR_INVALID_ARGUMENT;
    }
    return CHIP_ERROR_INTERNAL;
}

void SyncCoordinator::RemoveChangedAttributesListener(ChangedAttributesListener* listener)
{
    LockGuard g(mLock);
    mPublisher.Remove(listener);
}

// ─── Mutating entry points ────────────────────────────────────────────────────

OperationalChange SyncCoordinator::ApplyIntent(const WriteIntent& intent)
{
    OperationalChange         change;
    ChangePublisher::Snapshot snap;
    {
        LockGuard g(mLock);
        change = mKernel->applyIntent(intent);
        snap   = mPublisher.snapshot();
    }
    if (!change.dirtyAttributes.empty()) snap.notify(change.dirtyAttributes);
    if (change.sendCommand.has_value()) mPublisher.firePump();
    return change;
}

OperationalChange SyncCoordinator::ApplyOperationalObservation(
    const S21OperationalObservation& obs)
{
    OperationalChange         change;
    ChangePublisher::Snapshot snap;
    {
        LockGuard g(mLock);
        change = mKernel->applyOperationalObservation(obs);
        snap   = mPublisher.snapshot();
    }
    if (!change.dirtyAttributes.empty()) snap.notify(change.dirtyAttributes);
    if (change.sendCommand.has_value()) mPublisher.firePump();
    return change;
}

EnvironmentalChange SyncCoordinator::ApplyEnvironmentalObservation(
    const S21EnvironmentalObservation& obs)
{
    EnvironmentalChange       change;
    ChangePublisher::Snapshot snap;
    {
        LockGuard g(mLock);
        change = mKernel->applyEnvironmentalObservation(obs);
        snap   = mPublisher.snapshot();
    }
    if (!change.dirtyAttributes.empty()) snap.notify(change.dirtyAttributes);
    return change;
}

void SyncCoordinator::OnCommandSent(const S21OperationCommand& cmd)
{
    LockGuard g(mLock);
    mKernel->onCommandSent(cmd);
}

void SyncCoordinator::OnCommandFailed()
{
    LockGuard g(mLock);
    mKernel->onCommandFailed();
}

void SyncCoordinator::NotifyLinkDown()
{
    LockGuard g(mLock);
    mKernel->notifyLinkDown();
}

std::optional<S21OperationCommand> SyncCoordinator::PendingCommand() const
{
    LockGuard g(mLock);
    return mKernel->pendingCommand();
}

// ─── Per-attribute reads ─────────────────────────────────────────────────────

bool SyncCoordinator::ReadOnOff() const
{
    LockGuard g(mLock);
    return mKernel->readOnOff();
}

OperationalMode SyncCoordinator::ReadMode() const
{
    LockGuard g(mLock);
    return mKernel->readMode();
}

int16_t SyncCoordinator::ReadOccupiedHeatingSetpoint() const
{
    LockGuard g(mLock);
    return mKernel->readOccupiedHeatingSetpoint();
}

int16_t SyncCoordinator::ReadOccupiedCoolingSetpoint() const
{
    LockGuard g(mLock);
    return mKernel->readOccupiedCoolingSetpoint();
}

RunningMode SyncCoordinator::ReadRunningMode() const
{
    LockGuard g(mLock);
    return mKernel->readRunningMode();
}

std::optional<int16_t> SyncCoordinator::ReadLocalTemperature() const
{
    LockGuard g(mLock);
    return mKernel->readLocalTemperature();
}

std::optional<int16_t> SyncCoordinator::ReadOutdoorTemperature() const
{
    LockGuard g(mLock);
    return mKernel->readOutdoorTemperature();
}

ObservationSource SyncCoordinator::ReadSetpointSource() const
{
    LockGuard g(mLock);
    return mKernel->readSetpointSource();
}

std::optional<uint8_t> SyncCoordinator::ReadSpeedSetting() const
{
    LockGuard g(mLock);
    return mKernel->readSpeedSetting();
}

FanModeCategory SyncCoordinator::ReadFanMode() const
{
    LockGuard g(mLock);
    return mKernel->readFanMode();
}

uint8_t SyncCoordinator::ReadSpeedCurrent() const
{
    LockGuard g(mLock);
    return mKernel->readSpeedCurrent();
}

std::optional<uint8_t> SyncCoordinator::ReadPercentSetting() const
{
    LockGuard g(mLock);
    return mKernel->readPercentSetting();
}

uint8_t SyncCoordinator::ReadPercentCurrent() const
{
    LockGuard g(mLock);
    return mKernel->readPercentCurrent();
}

std::optional<uint16_t> SyncCoordinator::ReadHumidityCentiPercent() const
{
    LockGuard g(mLock);
    return mKernel->readHumidityCentiPercent();
}

bool SyncCoordinator::ReadReachable() const
{
    LockGuard g(mLock);
    return mKernel->readReachable();
}

ProjectedClusterState SyncCoordinator::ProjectionSnapshot() const
{
    LockGuard g(mLock);
    return mKernel->projectionSnapshot();
}

LogicalACState SyncCoordinator::Snapshot() const
{
    LockGuard g(mLock);
    return mKernel->snapshot();
}

} // namespace sync
