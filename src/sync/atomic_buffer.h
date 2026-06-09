/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#pragma once

#include "reconciler.h"
#include "time_source.h"
#include "write_intent.h"

#include <vector>

namespace sync {

/// Defined outside AtomicTxn so the constructor can default-initialise it
/// without falling foul of C++17's "complete class context" rule on the
/// outer class.
struct AtomicTxnConfig {
    int64_t timeoutMs = 5'000;
};

/**
 * AtomicTxn provides Matter Thermostat AtomicRequest support. Buffers
 * WriteIntents that arrive between AtomicRequest{Begin} and
 * AtomicRequest{Commit}, and applies them to the reconciler in a single
 * batch. Outside an open transaction, write() passes intents straight
 * through to the reconciler so the AAI path can be uniform.
 *
 * The Matter spec scopes AtomicRequest primarily to Presets/Schedules,
 * but the same surface is useful for any controller that wants to
 * atomically change both setpoints — the Auto-band translate case
 * (sync-option-4-plan Phase 2 / Group B / B9b). When commit() sees both
 * a heating-edge and a cooling-edge write in the buffer while the device
 * is in Auto mode, it forwards the pair as a single "set Auto target to
 * midpoint" operation, avoiding the centre drift of two non-atomic edits.
 *
 * Timeout policy: if the controller never sends Commit, the txn
 * auto-rolls back on the next operation (write/commit/rollback/isOpen
 * check) once the configured window has elapsed. The Matter spec
 * defaults to 5 s; we let the AAI inject the actual value from
 * Thermostat::Delegate::GetMaxAtomicWriteTimeout if available.
 *
 * Thread-safety: same as Reconciler — none on its own; the Phase 7
 * wrapper owns the mutex.
 */
class AtomicTxn {
public:
    enum class Status {
        Ok,            ///< Begin/Commit/Rollback succeeded or write was buffered
        AppliedNow,    ///< Outside-of-txn write was applied immediately
        AlreadyOpen,   ///< Begin called while a txn was already open
        NoneOpen,      ///< Commit/Rollback called with no open txn
        TimedOut,      ///< The open txn has exceeded the timeout
    };

    using Config = AtomicTxnConfig;

    AtomicTxn(Reconciler& rec, TimeSource& time, Config cfg = {});

    Status begin();
    Status write(const WriteIntent& intent);
    OperationalChange commit();
    Status rollback();

    bool   isOpen()        const { return mOpen; }
    size_t pendingCount()  const { return mBuffer.size(); }

private:
    Reconciler&              mRec;
    TimeSource&              mTime;
    Config                   mCfg;
    bool                     mOpen{false};
    int64_t                  mOpenedAtMs{0};
    std::vector<WriteIntent> mBuffer;

    bool hasTimedOut() const;
    /// If the txn has timed out, drop the buffer and return true.
    /// Callers use this at the top of public methods to fail closed.
    bool dropIfTimedOut();
};

} // namespace sync
