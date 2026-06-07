/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#include "atomic_buffer.h"

namespace sync {

AtomicTxn::AtomicTxn(Reconciler& rec, TimeSource& time, Config cfg)
    : mRec(rec), mTime(time), mCfg(cfg)
{
}

bool AtomicTxn::hasTimedOut() const
{
    if (!mOpen) return false;
    return (mTime.millis() - mOpenedAtMs) >= mCfg.timeoutMs;
}

bool AtomicTxn::dropIfTimedOut()
{
    if (!hasTimedOut()) return false;
    mBuffer.clear();
    mOpen = false;
    return true;
}

AtomicTxn::Status AtomicTxn::begin()
{
    // A timed-out txn auto-rolls-back so Begin can succeed on the new
    // attempt. Matter's AtomicRequest semantics require this; otherwise a
    // forgotten Begin from a dead session permanently blocks the cluster.
    dropIfTimedOut();
    if (mOpen) return Status::AlreadyOpen;

    mOpen       = true;
    mOpenedAtMs = mTime.millis();
    mBuffer.clear();
    return Status::Ok;
}

AtomicTxn::Status AtomicTxn::write(const WriteIntent& intent)
{
    if (dropIfTimedOut()) {
        // Fall through: write happens outside any txn, applied immediately.
    }

    if (mOpen) {
        mBuffer.push_back(intent);
        return Status::Ok;
    }
    mRec.applyIntent(intent);
    return Status::AppliedNow;
}

AppliedChange AtomicTxn::commit()
{
    if (dropIfTimedOut()) {
        // The buffered intents are gone; report nothing happened. Caller
        // (AAI) should translate this to a TIMEOUT status on the wire.
        return AppliedChange{};
    }
    if (!mOpen) return AppliedChange{};

    auto change = mRec.applyAtomicBundle(mBuffer);
    mBuffer.clear();
    mOpen = false;
    return change;
}

AtomicTxn::Status AtomicTxn::rollback()
{
    if (dropIfTimedOut()) return Status::TimedOut;
    if (!mOpen) return Status::NoneOpen;

    mBuffer.clear();
    mOpen = false;
    return Status::Ok;
}

} // namespace sync
