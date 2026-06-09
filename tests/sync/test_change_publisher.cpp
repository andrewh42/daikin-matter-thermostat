/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Tests for sync::ChangePublisher — listener registry, snapshot/notify,
 * and command-pump handler invocation.
 */

#include <catch2/catch_test_macros.hpp>

#include "change_publisher.h"

#include <vector>

using sync::ChangedAttributesListener;
using sync::ChangePublisher;
using sync::LogicalAttribute;

namespace {

struct RecordingListener : public ChangedAttributesListener {
    std::vector<std::vector<LogicalAttribute>> received;
    void OnChangedAttributes(const std::vector<LogicalAttribute>& attributes) override
    {
        received.push_back(attributes);
    }
};

struct SelfRemovingListener : public ChangedAttributesListener {
    ChangePublisher* publisher{nullptr};
    int              callCount{0};
    void OnChangedAttributes(const std::vector<LogicalAttribute>&) override
    {
        ++callCount;
        if (publisher) publisher->Remove(this);
    }
};

} // namespace

// ─── Registration & capacity ─────────────────────────────────────────────────

TEST_CASE("Add: nullptr returns InvalidArgument", "[change_publisher]")
{
    ChangePublisher pub;
    REQUIRE(pub.Add(nullptr) == ChangePublisher::Status::InvalidArgument);
    REQUIRE(pub.listenerCount() == 0);
}

TEST_CASE("Add: single listener succeeds and is reflected in count",
          "[change_publisher]")
{
    ChangePublisher  pub;
    RecordingListener l;
    REQUIRE(pub.Add(&l) == ChangePublisher::Status::Ok);
    REQUIRE(pub.listenerCount() == 1);
}

TEST_CASE("Add: same pointer twice returns AlreadyRegistered and stays idempotent",
          "[change_publisher]")
{
    ChangePublisher  pub;
    RecordingListener l;
    REQUIRE(pub.Add(&l) == ChangePublisher::Status::Ok);
    REQUIRE(pub.Add(&l) == ChangePublisher::Status::AlreadyRegistered);
    REQUIRE(pub.listenerCount() == 1);
}

TEST_CASE("Add: registering past kMaxListeners returns Full",
          "[change_publisher]")
{
    ChangePublisher pub;
    std::vector<RecordingListener> listeners(ChangePublisher::kMaxListeners + 1);
    for (size_t i = 0; i < ChangePublisher::kMaxListeners; ++i) {
        REQUIRE(pub.Add(&listeners[i]) == ChangePublisher::Status::Ok);
    }
    REQUIRE(pub.Add(&listeners.back()) == ChangePublisher::Status::Full);
    REQUIRE(pub.listenerCount() == ChangePublisher::kMaxListeners);
}

TEST_CASE("Remove: nullptr is a no-op", "[change_publisher]")
{
    ChangePublisher pub;
    RecordingListener l;
    pub.Add(&l);
    pub.Remove(nullptr);
    REQUIRE(pub.listenerCount() == 1);
}

TEST_CASE("Remove: unknown pointer is a no-op", "[change_publisher]")
{
    ChangePublisher pub;
    RecordingListener kept, stranger;
    pub.Add(&kept);
    pub.Remove(&stranger);
    REQUIRE(pub.listenerCount() == 1);
}

TEST_CASE("Remove: registered pointer is removed", "[change_publisher]")
{
    ChangePublisher pub;
    RecordingListener a, b;
    pub.Add(&a); pub.Add(&b);
    pub.Remove(&a);
    REQUIRE(pub.listenerCount() == 1);
    auto snap = pub.snapshot();
    REQUIRE(snap.count == 1);
    REQUIRE(snap.slots[0] == &b);
}

// ─── Snapshot & notify ───────────────────────────────────────────────────────

TEST_CASE("snapshot.notify fans out to every registered listener",
          "[change_publisher]")
{
    ChangePublisher pub;
    RecordingListener a, b;
    pub.Add(&a); pub.Add(&b);

    auto snap = pub.snapshot();
    const std::vector<LogicalAttribute> attrs = {LogicalAttribute::OnOff};
    snap.notify(attrs);

    REQUIRE(a.received.size() == 1);
    REQUIRE(a.received[0] == attrs);
    REQUIRE(b.received.size() == 1);
    REQUIRE(b.received[0] == attrs);
}

TEST_CASE("Snapshot preserves a stable view: Remove after snapshot doesn't unhook a fire",
          "[change_publisher]")
{
    // SyncCoordinator's pattern is: snapshot under lock, dispatch outside.
    // We assert that the snapshot captures the listener set at snap time,
    // independent of subsequent registry mutations.
    ChangePublisher pub;
    RecordingListener a, b;
    pub.Add(&a); pub.Add(&b);

    auto snap = pub.snapshot();
    pub.Remove(&b); // happens after the snapshot

    snap.notify({LogicalAttribute::OnOff});
    REQUIRE(a.received.size() == 1);
    // b was in the snapshot, so it must have been notified.
    REQUIRE(b.received.size() == 1);
}

TEST_CASE("A listener that calls Remove(this) during dispatch doesn't corrupt iteration",
          "[change_publisher]")
{
    // Pattern: the snapshot is fixed-size and pre-captured, so a listener
    // that removes itself from the publisher mid-dispatch doesn't disturb
    // the loop. The listener still receives the in-flight notify; the
    // next snapshot won't include it.
    ChangePublisher pub;
    SelfRemovingListener killer; killer.publisher = &pub;
    RecordingListener tail;
    pub.Add(&killer); pub.Add(&tail);

    auto snap = pub.snapshot();
    snap.notify({LogicalAttribute::OnOff});

    REQUIRE(killer.callCount == 1);
    REQUIRE(tail.received.size() == 1);
    REQUIRE(pub.listenerCount() == 1); // killer is gone post-dispatch

    auto next = pub.snapshot();
    next.notify({LogicalAttribute::SystemMode});
    REQUIRE(killer.callCount == 1); // not called again
    REQUIRE(tail.received.size() == 2);
}

// ─── Command pump ────────────────────────────────────────────────────────────

namespace {
int gPumpFires = 0;
void pumpHandler() { ++gPumpFires; }
} // namespace

TEST_CASE("firePump: no-op when no handler is set", "[change_publisher]")
{
    ChangePublisher pub;
    pub.firePump(); // must not crash
    SUCCEED();
}

TEST_CASE("firePump: invokes the registered handler exactly once per call",
          "[change_publisher]")
{
    ChangePublisher pub;
    gPumpFires = 0;
    pub.SetCommandPumpHandler(&pumpHandler);

    pub.firePump();
    pub.firePump();
    pub.firePump();

    REQUIRE(gPumpFires == 3);
}

TEST_CASE("firePump: clearing the handler stops further fires",
          "[change_publisher]")
{
    ChangePublisher pub;
    gPumpFires = 0;
    pub.SetCommandPumpHandler(&pumpHandler);
    pub.firePump();
    pub.SetCommandPumpHandler(nullptr);
    pub.firePump();

    REQUIRE(gPumpFires == 1);
}

// ─── Reset ───────────────────────────────────────────────────────────────────

TEST_CASE("Reset: clears listeners and pump handler", "[change_publisher]")
{
    ChangePublisher pub;
    RecordingListener l;
    pub.Add(&l);
    gPumpFires = 0;
    pub.SetCommandPumpHandler(&pumpHandler);

    pub.Reset();

    REQUIRE(pub.listenerCount() == 0);
    pub.firePump();
    REQUIRE(gPumpFires == 0);
}
