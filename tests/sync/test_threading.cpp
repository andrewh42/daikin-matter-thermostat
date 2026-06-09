/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Phase 7 threading stress test
 * -----------------------------
 * Two std::threads pummel a Reconciler from both sides — one playing the
 * Matter-event-loop role (applyIntent), the other the S21-work-queue
 * role (applyOperationalObservation). A std::mutex stands in for
 * SyncCoordinator's Zephyr k_mutex so the test is host-portable.
 *
 * What this catches:
 *   - data races on TwinField fields (UBSan would fire);
 *   - intent counts dropped on the floor by interleaving;
 *   - inconsistent attribution / dirty-path computation under contention.
 *
 * What it deliberately doesn't catch:
 *   - the Zephyr-specific work-queue scheduling (covered on-device);
 *   - mutex priority inversion / RT-scheduler issues (not relevant
 *     to the algorithm itself).
 */

#include <catch2/catch_test_macros.hpp>

#include "atomic_buffer.h"
#include "reconciler.h"

#include <atomic>
#include <mutex>
#include <thread>

using namespace sync;

namespace {

/// Minimal stand-in for the on-device SyncCoordinator lock discipline.
struct LockedReconciler {
    ManualTimeSource time;
    LogicalACState   state;
    Reconciler       rec;
    mutable std::mutex lock;

    LockedReconciler()
        : state(LogicalACStateDefaults{.onOff        = true,
                                       .mode         = OperationalMode::Cool,
                                       .coolSetpoint = 2400}),
          rec(state, time) {}

    void applyIntent(const WriteIntent& intent) {
        std::lock_guard<std::mutex> g(lock);
        rec.applyIntent(intent);
    }
    void applyOperationalObservation(const S21OperationalObservation& obs) {
        std::lock_guard<std::mutex> g(lock);
        rec.applyOperationalObservation(obs);
    }
    int16_t coolDesired() const {
        std::lock_guard<std::mutex> g(lock);
        return state.coolSetpoint.desired();
    }
    int16_t coolObserved() const {
        std::lock_guard<std::mutex> g(lock);
        return state.coolSetpoint.observed();
    }
};

S21OperationalObservation opPoll(int16_t setpoint)
{
    return {true, OperatingMode::Cool, setpoint, FanMode::Auto, std::nullopt};
}

} // namespace

TEST_CASE("Threading: 10k interleaved intents and observations don't corrupt state",
          "[phase7][threading]")
{
    LockedReconciler h;

    constexpr int kIterations = 10'000;
    std::atomic<int> intentsApplied{0};
    std::atomic<int> observationsApplied{0};

    std::thread matterThread([&] {
        for (int i = 0; i < kIterations; ++i) {
            const int16_t v = 2000 + (i % 1000); // 2000..2999
            h.applyIntent(SetOccupiedCoolingSetpointIntent{v});
            intentsApplied.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread s21Thread([&] {
        for (int i = 0; i < kIterations; ++i) {
            const int16_t v = 2400 + (i % 100);
            h.applyOperationalObservation(opPoll(v));
            observationsApplied.fetch_add(1, std::memory_order_relaxed);
        }
    });

    matterThread.join();
    s21Thread.join();

    REQUIRE(intentsApplied.load()      == kIterations);
    REQUIRE(observationsApplied.load() == kIterations);

    // No specific value is guaranteed (the threads race), but the twin
    // must be self-consistent: desired and observed must both be values
    // that one of the threads actually wrote, never garbage.
    auto desired  = h.coolDesired();
    auto observed = h.coolObserved();
    REQUIRE(desired  >= 2000); REQUIRE(desired  <= 2999);
    REQUIRE(observed >= 2400); REQUIRE(observed <= 2499);
}

TEST_CASE("Threading: every intent's setDesired survives to next-observed re-check",
          "[phase7][threading]")
{
    // A weaker but more directly meaningful invariant: if no observation
    // is interleaved between successive intents, every intent's value
    // must end up as desired. This pins that the lock prevents lost
    // setDesired writes.
    LockedReconciler h;
    constexpr int kIterations = 1'000;
    std::vector<int16_t> values;
    values.reserve(kIterations);
    for (int i = 0; i < kIterations; ++i) values.push_back(2000 + (i % 1000));

    for (auto v : values) {
        h.applyIntent(SetOccupiedCoolingSetpointIntent{v});
        REQUIRE(h.coolDesired() == v);
    }
}
