/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#pragma once

#include "changed_attributes_listener.h"
#include "logical_attribute.h"

#include <cstddef>
#include <vector>

namespace sync {

/**
 * ChangePublisher is the listener registry and dispatch helper for the
 * bridge's dirty-attribute and command-pump streams.
 *
 * The publisher itself does not own a lock: registration calls
 * (`Add`/`Remove`) and `snapshot()` are read/mutated only under
 * SyncCoordinator's mutex. `snapshot()` copies the listener vector into a
 * fixed-size stack-allocated struct so the dispatch loop runs outside
 * the lock — listeners may call `LockChipStack()` or schedule onto the
 * Matter event loop, both of which would deadlock if held under
 * SyncCoordinator's lock.
 *
 * CHIP-free: errors are returned as a small `Status` enum; SyncCoordinator
 * adapts at its public surface.
 */
class ChangePublisher {
public:
    /// Maximum number of listeners that may be registered at once. Sized
    /// for: Matter reporter, LED indicator, optional shell observer,
    /// optional metrics. Bump if you need more.
    static constexpr size_t kMaxListeners = 4;

    /// Single-consumer pump signal. Called by SyncCoordinator from mutation
    /// paths that produced a sendCommand. The publisher just keeps and
    /// fires the function pointer; the dispatch wraps it with the right
    /// "outside the lock" discipline.
    using CommandPumpHandler = void (*)();

    enum class Status { Ok, AlreadyRegistered, Full, InvalidArgument };

    void SetCommandPumpHandler(CommandPumpHandler h) { mPump = h; }

    /// Idempotent for the same pointer (returns AlreadyRegistered, which
    /// callers may treat as success).
    Status Add(ChangedAttributesListener* listener);
    void   Remove(ChangedAttributesListener* listener);

    size_t listenerCount() const { return mListeners.size(); }

    /// Stack-allocated snapshot of the listener vector. Captured under
    /// the caller's lock; notified outside it.
    struct Snapshot {
        ChangedAttributesListener* slots[kMaxListeners];
        size_t                     count{0};

        void notify(const std::vector<LogicalAttribute>& attributes) const
        {
            for (size_t i = 0; i < count; ++i) {
                slots[i]->OnChangedAttributes(attributes);
            }
        }
    };

    Snapshot snapshot() const;

    /// No-op when no handler is set.
    void firePump() const { if (mPump != nullptr) mPump(); }

    /// Reset publisher state to its constructed shape. Called by
    /// SyncCoordinator::Shutdown.
    void Reset();

private:
    std::vector<ChangedAttributesListener*> mListeners;
    CommandPumpHandler                      mPump{nullptr};
};

} // namespace sync
