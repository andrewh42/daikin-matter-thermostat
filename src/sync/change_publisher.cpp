/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#include "change_publisher.h"

#include <algorithm>

namespace sync {

ChangePublisher::Status ChangePublisher::Add(ChangedAttributesListener* listener)
{
    if (listener == nullptr) return Status::InvalidArgument;

    if (std::find(mListeners.begin(), mListeners.end(), listener) != mListeners.end()) {
        return Status::AlreadyRegistered;
    }
    if (mListeners.size() >= kMaxListeners) return Status::Full;

    // Caller is expected to have reserved capacity (SyncCoordinator does this in
    // its Init). The condition above guarantees no reallocation here.
    mListeners.push_back(listener);
    return Status::Ok;
}

void ChangePublisher::Remove(ChangedAttributesListener* listener)
{
    if (listener == nullptr) return;
    mListeners.erase(
        std::remove(mListeners.begin(), mListeners.end(), listener),
        mListeners.end());
}

ChangePublisher::Snapshot ChangePublisher::snapshot() const
{
    Snapshot snap;
    snap.count = mListeners.size();
    std::copy(mListeners.begin(), mListeners.end(), snap.slots);
    return snap;
}

void ChangePublisher::Reset()
{
    mListeners.clear();
    mPump = nullptr;
}

} // namespace sync
