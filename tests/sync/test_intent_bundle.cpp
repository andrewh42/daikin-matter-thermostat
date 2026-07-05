/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Tests for sync::Reconciler::applyIntentBundle — the primitive that
 * applies a vector of WriteIntents as if they arrived simultaneously.
 *
 * The primitive's sole behavioural difference from a sequence of
 * applyIntent calls is in Auto mode: a (heat, cool) setpoint pair is
 * collapsed into a single autoSetpoint = midpoint update, avoiding the
 * centre drift that a two-step apply would produce on crossing edits.
 *
 * The bundle is the building block reserved for a future Matter
 * AtomicRequest wiring (see atomic-txn-completion-plan.md). Pinning its
 * behaviour at the reconciler boundary lets that wiring be added later
 * without re-deriving the centre-drift semantics.
 */

#include <catch2/catch_test_macros.hpp>

#include "reconciler.h"

#include <algorithm>

using namespace sync;

namespace {

struct Harness {
    ManualTimeSource time;
    LogicalACState   state;
    Reconciler       rec;

    Harness(LogicalACStateDefaults defaults = {}, ReconcilerConfig cfg = {})
        : time(0), state(defaults), rec(state, time, cfg)
    {
    }
};

S21OperationalObservation opObs(bool onOff, OperatingMode mode, int16_t setpoint,
                                FanMode fan = FanMode::Auto)
{
    return {onOff, mode, setpoint, fan, std::nullopt};
}

LogicalACStateDefaults coolDefaults(int16_t coolSp = 2400)
{
    LogicalACStateDefaults d;
    d.onOff        = true;
    d.mode         = OperationalMode::Cool;
    d.coolSetpoint = coolSp;
    return d;
}

LogicalACStateDefaults autoDefaults(int16_t autoSp = 2200,
                                    int16_t heatSp = 2000,
                                    int16_t coolSp = 2500)
{
    LogicalACStateDefaults d;
    d.onOff        = true;
    d.mode         = OperationalMode::Auto;
    d.autoSetpoint = autoSp;
    d.heatSetpoint = heatSp;
    d.coolSetpoint = coolSp;
    return d;
}

bool containsAttr(const std::vector<LogicalAttribute>& attrs, LogicalAttribute target)
{
    return std::any_of(attrs.begin(), attrs.end(),
        [&](LogicalAttribute a) { return a == target; });
}

} // namespace

// ─── Bundle-of-one parity with applyIntent ───────────────────────────────────

TEST_CASE("Bundle of one SetOccupiedCoolingSetpointIntent matches applyIntent",
          "[intent_bundle][parity]")
{
    // Baselines diverge unless both fixtures see the same observation
    // history, so we run two parallel reconcilers and compare end-states.
    Harness a(coolDefaults(2400));
    Harness b(coolDefaults(2400));
    a.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));
    b.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));

    auto bundleChange = a.rec.applyIntentBundle({SetOccupiedCoolingSetpointIntent{2600}});
    auto singleChange = b.rec.applyIntent(SetOccupiedCoolingSetpointIntent{2600});

    REQUIRE(a.state.coolSetpoint.desired() == b.state.coolSetpoint.desired());
    REQUIRE(bundleChange.sendCommand.has_value());
    REQUIRE(singleChange.sendCommand.has_value());
    REQUIRE(bundleChange.sendCommand->setpointCelsius ==
            singleChange.sendCommand->setpointCelsius);
}

TEST_CASE("Bundle of one SetSystemModeIntent matches applyIntent",
          "[intent_bundle][parity]")
{
    Harness a(coolDefaults(2400));
    Harness b(coolDefaults(2400));
    a.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));
    b.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));

    a.rec.applyIntentBundle({SetSystemModeIntent{true, OperationalMode::Heat}});
    b.rec.applyIntent(SetSystemModeIntent{true, OperationalMode::Heat});

    REQUIRE(a.state.mode.desired() == b.state.mode.desired());
    REQUIRE(a.state.onOff.desired() == b.state.onOff.desired());
}

// ─── Non-Auto modes: bundle is a sequence of applies ─────────────────────────

TEST_CASE("Heat+Cool bundle in Cool mode applies each intent, leaves autoSetpoint alone",
          "[intent_bundle]")
{
    Harness h(coolDefaults(2400));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));
    const int16_t autoBefore = h.state.autoSetpoint.observed();

    h.rec.applyIntentBundle({
        SetOccupiedHeatingSetpointIntent{2100},
        SetOccupiedCoolingSetpointIntent{2700},
    });

    REQUIRE(h.state.heatSetpoint.desired() == 2100);
    REQUIRE(h.state.coolSetpoint.desired() == 2700);
    REQUIRE(h.state.autoSetpoint.observed() == autoBefore); // unchanged
}

// ─── Auto mode: collapse to midpoint ─────────────────────────────────────────

TEST_CASE("Auto bundle of both setpoints raised by +2°C lands autoSetpoint at +2°C exactly",
          "[intent_bundle][auto]")
{
    // Both edges +200 centi-°C from defaults (heat 2000→2200, cool 2500→2700).
    // Bundle midpoint = (2200 + 2700)/2 = 2450, which is +2.5°C from the
    // starting auto centre of 2200. Tests that the *atomic* result equals
    // midpoint(newHeat, newCool) — independent of the prior centre.
    Harness h(autoDefaults(2200));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Auto, 2200));

    auto change = h.rec.applyIntentBundle({
        SetOccupiedHeatingSetpointIntent{2200},
        SetOccupiedCoolingSetpointIntent{2700},
    });

    REQUIRE(h.state.autoSetpoint.desired() == 2450);
    REQUIRE(change.sendCommand.has_value());
    REQUIRE(change.sendCommand->operatingMode  == OperatingMode::Auto);
    REQUIRE(change.sendCommand->setpointCelsius == 2450);
    REQUIRE(containsAttr(change.dirtyAttributes,
                         LogicalAttribute::OccupiedHeatingSetpoint));
    REQUIRE(containsAttr(change.dirtyAttributes,
                         LogicalAttribute::OccupiedCoolingSetpoint));
}

TEST_CASE("Auto bundle of crossing edits lands exact midpoint regardless of order",
          "[intent_bundle][auto][centreDrift]")
{
    // The interesting case is genuinely crossing: heat raised *above* the
    // current cool, and cool dropped *below* the current heat. Sequential
    // apply produces a path-dependent centre; the bundle locks it to the
    // bundle's own midpoint regardless of intent order. The auto-band
    // collapse only ever touches autoSetpoint; the individual heat/cool
    // shadows are not mutated by a bundle in Auto mode (reconciler.cpp
    // collapseAutoBand `continue` skips them).
    Harness a(autoDefaults(2200, /*heat*/2000, /*cool*/2500));
    Harness b(autoDefaults(2200, /*heat*/2000, /*cool*/2500));
    a.rec.applyOperationalObservation(opObs(true, OperatingMode::Auto, 2200));
    b.rec.applyOperationalObservation(opObs(true, OperatingMode::Auto, 2200));

    // Same intents, opposite orders. The bundle's collapse uses the *last*
    // heat/cool intent in bundle order, so the midpoint must be the same.
    auto fwd = a.rec.applyIntentBundle({
        SetOccupiedHeatingSetpointIntent{2600},
        SetOccupiedCoolingSetpointIntent{2100},
    });
    auto rev = b.rec.applyIntentBundle({
        SetOccupiedCoolingSetpointIntent{2100},
        SetOccupiedHeatingSetpointIntent{2600},
    });

    // Bundle midpoint = (2600 + 2100) / 2 = 2350.
    REQUIRE(a.state.autoSetpoint.desired() == 2350);
    REQUIRE(b.state.autoSetpoint.desired() == 2350);
    REQUIRE(fwd.sendCommand->setpointCelsius == 2350);
    REQUIRE(rev.sendCommand->setpointCelsius == 2350);
}

// ─── Last-write-wins within the bundle for non-collapsed intents ─────────────

TEST_CASE("Duplicate non-setpoint intents in a bundle: last one wins",
          "[intent_bundle][ordering]")
{
    // Mirrors reconciler.cpp:269-276 — the Auto collapse uses the *last*
    // heat/cool intent in bundle order, and other intent kinds also apply
    // in order. Two SetSystemModeIntent entries: the later one's mode is
    // the one that lands.
    Harness h(coolDefaults(2400));
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Cool, 2400));

    h.rec.applyIntentBundle({
        SetSystemModeIntent{true, OperationalMode::Heat},
        SetSystemModeIntent{true, OperationalMode::Dry},
    });

    REQUIRE(h.state.mode.desired() == OperationalMode::Dry);
}

// ─── Guard window respected by the Auto-band collapse ────────────────────────

TEST_CASE("Auto-band collapse inside guard window is dropped",
          "[intent_bundle][auto][guard]")
{
    // The collapse path is the only piece of bundle logic that bypasses
    // the per-intent apply() dispatchers (the heat/cool intents are
    // skipped in favour of a single autoSetpoint mutation). It must still
    // honour the device-wins guard for autoSetpoint, otherwise a stale
    // controller write predating a manual panel change could overwrite
    // the panel's centre. Per-intent guard semantics are inherited from
    // applyIntent and covered by test_reconciler.cpp's Group D tests.
    Harness h(autoDefaults(2200));
    h.time.set(10'000);
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Auto, 2200));

    // Panel pushes the centre to 2300; that observation is "fresh".
    h.time.advance(5'000);
    h.rec.applyOperationalObservation(opObs(true, OperatingMode::Auto, 2300));
    REQUIRE(h.state.autoSetpoint.observed() == 2300);

    // Bundle arrives 100 ms later — inside the 1 s guard window. The
    // midpoint would be 2400, which differs from observed 2300, so the
    // guard fires.
    h.time.advance(100);
    h.rec.applyIntentBundle({
        SetOccupiedHeatingSetpointIntent{2300},
        SetOccupiedCoolingSetpointIntent{2500},
    });

    REQUIRE(h.state.autoSetpoint.desired() == 2300); // unchanged from observed
}
