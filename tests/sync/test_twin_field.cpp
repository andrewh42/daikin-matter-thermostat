/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Tests for sync::TwinField<T>.
 */

#include <catch2/catch_test_macros.hpp>

#include "twin_field.h"

using sync::TwinField;
using Source = TwinField<int>::Source;

TEST_CASE("Initial state is observed-only", "[phase1][twin_field]")
{
    TwinField<int> t(20);

    REQUIRE(t.observed() == 20);
    REQUIRE(t.desired() == 20);
    REQUIRE_FALSE(t.inFlight().has_value());
    REQUIRE_FALSE(t.dirty());
    REQUIRE(t.lastObservedSource() == Source::Boot);
}

TEST_CASE("Desire diverges without affecting observed", "[phase1][twin_field]")
{
    TwinField<int> t(20);
    t.setDesired(22);

    REQUIRE(t.observed() == 20);
    REQUIRE(t.desired()  == 22);
    REQUIRE_FALSE(t.inFlight().has_value());
    REQUIRE(t.dirty());
}

TEST_CASE("Promotion to in-flight stops the field being dirty", "[phase1][twin_field]")
{
    TwinField<int> t(20);
    t.setDesired(22);
    t.promoteDesiredToInFlight();

    REQUIRE(t.inFlight().has_value());
    REQUIRE(*t.inFlight() == 22);
    REQUIRE(t.desired()   == 22);
    REQUIRE(t.observed()  == 20); // not yet confirmed
    REQUIRE_FALSE(t.dirty());
}

TEST_CASE("Confirmation by observation clears in-flight", "[phase1][twin_field]")
{
    TwinField<int> t(20);
    t.setDesired(22);
    t.promoteDesiredToInFlight();
    t.applyObservation(22, Source::Device);

    REQUIRE(t.observed() == 22);
    REQUIRE(t.desired()  == 22);
    REQUIRE_FALSE(t.inFlight().has_value());
    REQUIRE_FALSE(t.dirty());
    REQUIRE(t.lastObservedSource() == Source::Device);
}

TEST_CASE("Disconfirmation: clamp updates observed but preserves desired",
          "[phase1][twin_field]")
{
    // Device clamps our 35 down to 32. We accept the device value as
    // authoritative for *observed* (someone else is in charge), but we
    // leave desired alone: a controller may have queued a further intent
    // during the in-flight, and we don't want to silently drop it. The
    // reconciler's dedup against mLastSentCommand prevents a retry loop.
    TwinField<int> t(30);
    t.setDesired(35);
    t.promoteDesiredToInFlight();
    t.applyObservation(32, Source::Device);

    REQUIRE(t.observed() == 32);
    REQUIRE(t.desired()  == 35);
    REQUIRE_FALSE(t.inFlight().has_value());
    REQUIRE(t.dirty()); // desired != observed; reconciler dedup handles retry
    REQUIRE(t.lastObservedSource() == Source::Device);
}

TEST_CASE("Stale poll (matches pre-send observed) leaves state alone",
          "[phase1][twin_field]")
{
    // Plan A1: pre-send observed 24, sent 26, poll returns 24 (snapshot
    // predates the write). Nothing about state should change.
    TwinField<int> t(24);
    t.setDesired(26);
    t.promoteDesiredToInFlight();
    t.applyObservation(24, Source::Device);

    REQUIRE(t.observed() == 24);
    REQUIRE(t.desired()  == 26);
    REQUIRE(t.inFlight().has_value());
    REQUIRE(*t.inFlight() == 26);
}

TEST_CASE("External change without pending in-flight", "[phase1][twin_field]")
{
    // Remote panel turns something up to 24.
    TwinField<int> t(20);
    t.applyObservation(24, Source::Device);

    REQUIRE(t.observed() == 24);
    REQUIRE(t.desired()  == 24);
    REQUIRE_FALSE(t.dirty());
    REQUIRE(t.lastObservedSource() == Source::Device);
}

TEST_CASE("Idempotent observation is a no-op for dirty/desired but refreshes source",
          "[phase1][twin_field]")
{
    TwinField<int> t(20);
    t.setDesired(22);              // pending intent — desired diverges
    REQUIRE(t.dirty());
    t.applyObservation(20, Source::Device); // same as observed

    // The pending Matter intent must NOT be wiped by a confirming poll of
    // the old value; only inFlight/observed paths overwrite desired.
    REQUIRE(t.observed() == 20);
    REQUIRE(t.desired()  == 22);
    REQUIRE(t.dirty());
    REQUIRE(t.lastObservedSource() == Source::Device);
}

TEST_CASE("Matter-source observation is recorded", "[phase1][twin_field]")
{
    TwinField<int> t(20);
    t.applyObservation(21, Source::Matter);
    REQUIRE(t.lastObservedSource() == Source::Matter);
}
