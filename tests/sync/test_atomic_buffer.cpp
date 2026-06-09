/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Tests for sync::AtomicTxn — buffering of WriteIntents inside an
 * AtomicRequest{Begin/Commit/Rollback} transaction.
 */

#include <catch2/catch_test_macros.hpp>

#include "atomic_buffer.h"

using namespace sync;

namespace {

struct Harness {
    ManualTimeSource  time;
    LogicalACState    state;
    Reconciler        rec;
    AtomicTxn         txn;

    Harness(LogicalACStateDefaults d = {},
            AtomicTxn::Config      ac = AtomicTxn::Config{})
        : time(0), state(d), rec(state, time), txn(rec, time, ac)
    {
    }
};

LogicalACStateDefaults coolDefaults(int16_t coolSp = 2400)
{
    LogicalACStateDefaults d;
    d.onOff        = true;
    d.mode         = OperationalMode::Cool;
    d.coolSetpoint = coolSp;
    return d;
}

LogicalACStateDefaults autoDefaults(int16_t autoSp = 2200)
{
    LogicalACStateDefaults d;
    d.onOff        = true;
    d.mode         = OperationalMode::Auto;
    d.autoSetpoint = autoSp;
    d.heatSetpoint = 2000;
    d.coolSetpoint = 2500;
    return d;
}

} // namespace

// ─── Plan tests ──────────────────────────────────────────────────────────────

TEST_CASE("Begin → write → write → Commit applies all writes at once",
          "[phase4][atomic]")
{
    Harness h(coolDefaults(2400));
    REQUIRE(h.txn.begin() == AtomicTxn::Status::Ok);

    REQUIRE(h.txn.write(SetOccupiedCoolingSetpointIntent{2600}) == AtomicTxn::Status::Ok);
    REQUIRE(h.state.coolSetpoint.desired() == 2400); // buffered, not yet applied

    REQUIRE(h.txn.write(SetSystemModeIntent{true, OperationalMode::Heat}) == AtomicTxn::Status::Ok);
    REQUIRE(h.state.mode.desired() == OperationalMode::Cool); // still buffered

    auto change = h.txn.commit();

    REQUIRE(h.state.coolSetpoint.desired() == 2600);
    REQUIRE(h.state.mode.desired()         == OperationalMode::Heat);
    REQUIRE_FALSE(h.txn.isOpen());
    REQUIRE(change.sendCommand.has_value());
}

TEST_CASE("Begin → write → Rollback discards writes; twin unchanged",
          "[phase4][atomic]")
{
    Harness h(coolDefaults(2400));
    REQUIRE(h.txn.begin() == AtomicTxn::Status::Ok);
    h.txn.write(SetOccupiedCoolingSetpointIntent{2600});

    REQUIRE(h.txn.rollback() == AtomicTxn::Status::Ok);

    REQUIRE(h.state.coolSetpoint.desired() == 2400);
    REQUIRE_FALSE(h.txn.isOpen());
    REQUIRE(h.txn.pendingCount() == 0);
}

TEST_CASE("Writes outside an open transaction apply immediately",
          "[phase4][atomic]")
{
    Harness h(coolDefaults(2400));

    REQUIRE(h.txn.write(SetOccupiedCoolingSetpointIntent{2600})
            == AtomicTxn::Status::AppliedNow);
    REQUIRE(h.state.coolSetpoint.desired() == 2600);
}

TEST_CASE("Begin while a txn is already open is rejected",
          "[phase4][atomic]")
{
    Harness h(coolDefaults());
    REQUIRE(h.txn.begin() == AtomicTxn::Status::Ok);
    REQUIRE(h.txn.begin() == AtomicTxn::Status::AlreadyOpen);
    REQUIRE(h.txn.isOpen());
}

TEST_CASE("Commit/Rollback without an open txn is a no-op (Status::NoneOpen)",
          "[phase4][atomic]")
{
    Harness h(coolDefaults());
    auto change = h.txn.commit();
    REQUIRE_FALSE(change.sendCommand.has_value());
    REQUIRE(change.dirtyAttributes.empty());

    REQUIRE(h.txn.rollback() == AtomicTxn::Status::NoneOpen);
}

TEST_CASE("Timed-out transaction is auto-closed on next operation",
          "[phase4][atomic]")
{
    Harness h(coolDefaults(2400), AtomicTxn::Config{.timeoutMs = 1000});

    REQUIRE(h.txn.begin() == AtomicTxn::Status::Ok);
    h.txn.write(SetOccupiedCoolingSetpointIntent{2600});

    h.time.advance(1'500); // > 1000ms timeout

    // Commit after timeout: buffer dropped, no apply, no sendCommand.
    auto change = h.txn.commit();
    REQUIRE(h.state.coolSetpoint.desired() == 2400);
    REQUIRE_FALSE(change.sendCommand.has_value());
    REQUIRE_FALSE(h.txn.isOpen());
}

TEST_CASE("After timeout, a new Begin succeeds (txn slot is freed)",
          "[phase4][atomic]")
{
    Harness h(coolDefaults(2400), AtomicTxn::Config{.timeoutMs = 1000});
    h.txn.begin();
    h.time.advance(2'000);

    REQUIRE(h.txn.begin() == AtomicTxn::Status::Ok);
    REQUIRE(h.txn.isOpen());
}

// ─── Band-translate (B9b spec gap, now closed by atomic) ─────────────────────

TEST_CASE("Atomic Heat+Cool edits in Auto collapse to exact midpoint",
          "[phase4][atomic][groupB]")
{
    Harness h(autoDefaults(2200));
    REQUIRE(h.txn.begin() == AtomicTxn::Status::Ok);

    // Both edges +2°C: heat 21.5→23.5, cool 22.5→24.5.
    // Expected centre = (2350 + 2450) / 2 = 2400 — exactly +2°C from
    // the original 2200, with no two-step centre drift.
    h.txn.write(SetOccupiedHeatingSetpointIntent{2350});
    h.txn.write(SetOccupiedCoolingSetpointIntent{2450});
    auto change = h.txn.commit();

    REQUIRE(h.state.autoSetpoint.desired() == 2400);
    REQUIRE(change.sendCommand.has_value());
    REQUIRE(change.sendCommand->setpointCelsius == 2400);
}

TEST_CASE("Atomic single-edge edit in Auto behaves like a normal apply",
          "[phase4][atomic][groupB]")
{
    Harness h(autoDefaults(2200));
    REQUIRE(h.txn.begin() == AtomicTxn::Status::Ok);

    h.txn.write(SetOccupiedHeatingSetpointIntent{1900});
    auto change = h.txn.commit();

    // Same outcome as the non-atomic path (Phase 2 B7): centre =
    // midpoint(currentCool, newHeat) = (2250 + 1900)/2 = 2075.
    REQUIRE(h.state.autoSetpoint.desired() == 2075);
    REQUIRE(change.sendCommand->setpointCelsius == 2075);
}
