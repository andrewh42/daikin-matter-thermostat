/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#include <catch2/catch_test_macros.hpp>

#include "s21_manager.h"
#include "mock_s21_presentation_sync.h"

// ─── Fixture helpers ──────────────────────────────────────────────────────────

static S21PresentationError presError(const char* msg)
{
    return S21PresentationError{msg};
}

static S21DataLinkError dlError(const char* msg)
{
    return S21DataLinkError{msg};
}

struct Fixture {
    MockS21PresentationSync mock;

    // Injectable fake clock: controls what 'now' is for cache tests.
    S21Manager::TimePoint fakeNow{S21Manager::Clock::now()};
    S21Manager::TimeSource fakeClock{[this] { return fakeNow; }};

    // Manager uses 10-second cache, fake clock.
    S21Manager mgr{mock, std::chrono::seconds{10}, fakeClock};

    // Bring mgr to Ready with a v1 unit (single F8 call, no FY00).
    void initV1()
    {
        mock.protocolVersionResult = {{1, 0}};
        mgr.Init();
    }

    // Bring mgr to Ready with a v2 unit (F8 + FY00).
    void initV2()
    {
        mock.protocolVersionResult    = {{2, 0}};
        mock.extProtocolVersionResult = {{2, 1}};
        mgr.Init();
    }

    // Make the operation cache stale by advancing fake time past maxAge.
    void expireOperationCache() { fakeNow += std::chrono::seconds{11}; }

    // Make coarse cache stale.
    void expireCoarseCache() { fakeNow += std::chrono::seconds{11}; }

    // Drive a command capability to Unsupported (kThreshold + 1 failures).
    // Returns the error that was set on the presentation before each failure call.
    void driveUnsupported(std::function<S21Manager::Result<int16_t>()> getter,
                          tl::expected<int16_t, S21PresentationError>& resultField)
    {
        auto err = tl::unexpected(presError("error"));
        resultField = err;
        for (int i = 0; i <= 3; ++i) {
            getter();
        }
    }
};

// ─── Init & state machine ─────────────────────────────────────────────────────

TEST_CASE("S21Manager getOnOff returns not-ready before Init()", "[s21mgr][init]")
{
    Fixture f;
    auto r = f.mgr.getOnOff();
    REQUIRE_FALSE(r.has_value());
    REQUIRE(std::string_view{r.error().message} == "not ready");
}

TEST_CASE("S21Manager setOperation returns not-ready before Init()", "[s21mgr][init]")
{
    Fixture f;
    auto r = f.mgr.setOperation(true, OperatingMode::Cool, 2200, FanMode::Auto);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(std::string_view{r.error().message} == "not ready");
}

TEST_CASE("S21Manager v1 unit: Init() calls getProtocolVersion only", "[s21mgr][init]")
{
    Fixture f;
    f.mock.protocolVersionResult = {{1, 0}};

    f.mgr.Init();

    REQUIRE(f.mgr.isReady());
    REQUIRE(f.mock.getProtocolVersionCallCount == 1);
    REQUIRE(f.mock.getExtProtocolVersionCallCount == 0);
}

TEST_CASE("S21Manager v2 unit: Init() calls getProtocolVersion and getExtendedProtocolVersion",
          "[s21mgr][init]")
{
    Fixture f;
    f.mock.protocolVersionResult    = {{2, 0}};
    f.mock.extProtocolVersionResult = {{2, 1}};

    f.mgr.Init();

    REQUIRE(f.mgr.isReady());
    REQUIRE(f.mock.getProtocolVersionCallCount == 1);
    REQUIRE(f.mock.getExtProtocolVersionCallCount == 1);
}

TEST_CASE("S21Manager v3 unit: extended version updates protocol major", "[s21mgr][init]")
{
    Fixture f;
    f.mock.protocolVersionResult    = {{2, 0}};  // legacy F8 returns 2.0 for v3+ units
    f.mock.extProtocolVersionResult = {{3, 2}};

    f.mgr.Init();

    REQUIRE(f.mgr.isReady());
    REQUIRE(f.mock.getProtocolVersionCallCount == 1);
    REQUIRE(f.mock.getExtProtocolVersionCallCount == 1);
}

TEST_CASE("S21Manager getProtocolVersion failure: Init() returns < 0, not ready",
          "[s21mgr][init]")
{
    Fixture f;
    f.mock.protocolVersionResult = tl::unexpected(presError("timeout"));

    int err = f.mgr.Init();

    REQUIRE(err < 0);
    REQUIRE_FALSE(f.mgr.isReady());
    REQUIRE(f.mock.getProtocolVersionCallCount == 1);
    REQUIRE(f.mock.getExtProtocolVersionCallCount == 0);
}

TEST_CASE("S21Manager getProtocolVersion failure: Init() is retryable", "[s21mgr][init]")
{
    Fixture f;
    f.mock.protocolVersionResult = tl::unexpected(presError("timeout"));

    // First attempt fails
    REQUIRE(f.mgr.Init() < 0);
    REQUIRE_FALSE(f.mgr.isReady());

    // Second attempt also fails (idempotent guard should NOT fire when not ready)
    REQUIRE(f.mgr.Init() < 0);
    REQUIRE_FALSE(f.mgr.isReady());
    REQUIRE(f.mock.getProtocolVersionCallCount == 2);

    // Third attempt succeeds once the AC comes up
    f.mock.protocolVersionResult = {{1, 0}};
    REQUIRE(f.mgr.Init() == 0);
    REQUIRE(f.mgr.isReady());
    REQUIRE(f.mock.getProtocolVersionCallCount == 3);
}

TEST_CASE("S21Manager v2 with ext version NAK: still reaches Ready", "[s21mgr][init]")
{
    Fixture f;
    f.mock.protocolVersionResult    = {{2, 0}};
    f.mock.extProtocolVersionResult = tl::unexpected(presError("NAK"));

    f.mgr.Init();

    REQUIRE(f.mgr.isReady());
}

TEST_CASE("S21Manager Init() is idempotent", "[s21mgr][init]")
{
    Fixture f;
    f.initV1();
    f.mgr.Init();
    f.mgr.Init();

    REQUIRE(f.mock.getProtocolVersionCallCount == 1);
}

// ─── Operation cache ──────────────────────────────────────────────────────────

TEST_CASE("S21Manager getOnOff triggers lazy getOperation on first call", "[s21mgr][cache]")
{
    Fixture f;
    f.initV1();

    f.mock.operationResult = {std::make_tuple(true, OperatingMode::Cool, 2200, FanMode::Auto)};
    auto r = f.mgr.getOnOff();

    REQUIRE(r.has_value());
    REQUIRE(*r == true);
    REQUIRE(f.mock.getOperationCallCount == 1);
}

TEST_CASE("S21Manager second getOnOff uses cache — no second getOperation call", "[s21mgr][cache]")
{
    Fixture f;
    f.initV1();

    f.mgr.getOnOff();
    f.mgr.getOnOff();

    REQUIRE(f.mock.getOperationCallCount == 1);
}

TEST_CASE("S21Manager expired cache triggers new getOperation", "[s21mgr][cache]")
{
    Fixture f;
    f.initV1();

    f.mgr.getOnOff();
    REQUIRE(f.mock.getOperationCallCount == 1);

    f.expireOperationCache();
    f.mgr.getOnOff();
    REQUIRE(f.mock.getOperationCallCount == 2);
}

TEST_CASE("S21Manager setOperation success updates cache; getOnOff returns new value",
          "[s21mgr][cache]")
{
    Fixture f;
    f.initV1();

    // Prime cache with off
    f.mock.operationResult = {std::make_tuple(false, OperatingMode::Cool, 2200, FanMode::Auto)};
    f.mgr.getOnOff();
    REQUIRE(f.mock.getOperationCallCount == 1);

    // setOperation succeeds
    f.mgr.setOperation(true, OperatingMode::Heat, 2500, FanMode::Low);

    // getOnOff should return updated value without another getOperation call
    auto r = f.mgr.getOnOff();
    REQUIRE(r.has_value());
    REQUIRE(*r == true);
    REQUIRE(f.mock.getOperationCallCount == 1);
}

TEST_CASE("S21Manager setOperation failure: cache not updated; next getOnOff re-fetches",
          "[s21mgr][cache]")
{
    Fixture f;
    f.initV1();

    f.mock.operationResult = {std::make_tuple(false, OperatingMode::Cool, 2200, FanMode::Auto)};
    f.mgr.getOnOff();
    REQUIRE(f.mock.getOperationCallCount == 1);

    f.mock.setOperationResult = tl::unexpected(dlError("NAK"));
    f.mgr.setOperation(true, OperatingMode::Cool, 2200, FanMode::Auto);

    // Expire cache so we know re-fetch would happen
    f.expireOperationCache();
    f.mgr.getOnOff();
    REQUIRE(f.mock.getOperationCallCount == 2);
}

TEST_CASE("S21Manager coarse fallbacks share coarse cache", "[s21mgr][cache]")
{
    Fixture f;
    f.initV1();

    // Make room, outdoor, and humidity all fail so they fall back to coarse
    f.mock.roomTempResult    = tl::unexpected(presError("error"));
    f.mock.outdoorTempResult = tl::unexpected(presError("error"));
    f.mock.humidityResult    = tl::unexpected(presError("error"));
    f.mock.coarseResult      = {std::make_tuple<int16_t, int16_t, uint8_t>(2350, 3000, 50)};

    f.mgr.getRoomTemperature();
    f.mgr.getOutdoorTemperature();
    f.mgr.getHumidity();

    // Only one coarse fetch for all three
    REQUIRE(f.mock.getCoarseCallCount == 1);
}

TEST_CASE("S21Manager coarse cache expires and re-fetches", "[s21mgr][cache]")
{
    Fixture f;
    f.initV1();

    f.mock.roomTempResult = tl::unexpected(presError("error"));
    f.mock.coarseResult   = {std::make_tuple<int16_t, int16_t, uint8_t>(2350, 3000, 50)};

    f.mgr.getRoomTemperature();
    REQUIRE(f.mock.getCoarseCallCount == 1);

    f.expireCoarseCache();
    f.mgr.getRoomTemperature();
    REQUIRE(f.mock.getCoarseCallCount == 2);
}

// ─── Getters from operation cache ─────────────────────────────────────────────

TEST_CASE("S21Manager getOnOff returns correct value", "[s21mgr][get]")
{
    Fixture f;
    f.initV1();
    f.mock.operationResult = {std::make_tuple(true, OperatingMode::Heat, 2500, FanMode::Low)};

    REQUIRE(*f.mgr.getOnOff() == true);
}

TEST_CASE("S21Manager getOperatingMode returns correct value", "[s21mgr][get]")
{
    Fixture f;
    f.initV1();
    f.mock.operationResult = {std::make_tuple(true, OperatingMode::Heat, 2500, FanMode::Low)};

    REQUIRE(*f.mgr.getOperatingMode() == OperatingMode::Heat);
}

TEST_CASE("S21Manager getSetpoint returns correct value", "[s21mgr][get]")
{
    Fixture f;
    f.initV1();
    f.mock.operationResult = {std::make_tuple(true, OperatingMode::Cool, 2350, FanMode::Auto)};

    REQUIRE(*f.mgr.getSetpoint() == 2350);
}

TEST_CASE("S21Manager getOperation error propagates to getOnOff", "[s21mgr][get]")
{
    Fixture f;
    f.initV1();
    f.mock.operationResult = tl::unexpected(presError("timeout"));

    auto r = f.mgr.getOnOff();
    REQUIRE_FALSE(r.has_value());
    REQUIRE(std::string_view{r.error().message} == "timeout");
}

TEST_CASE("S21Manager getOperatingMode propagates getOperation error", "[s21mgr][get]")
{
    Fixture f;
    f.initV1();
    f.mock.operationResult = tl::unexpected(presError("timeout"));

    REQUIRE_FALSE(f.mgr.getOperatingMode().has_value());
}

TEST_CASE("S21Manager getSetpoint propagates getOperation error", "[s21mgr][get]")
{
    Fixture f;
    f.initV1();
    f.mock.operationResult = tl::unexpected(presError("timeout"));

    REQUIRE_FALSE(f.mgr.getSetpoint().has_value());
}

// ─── Fallback: getRoomTemperature ─────────────────────────────────────────────

TEST_CASE("S21Manager getRoomTemperature returns RH value on success", "[s21mgr][fallback]")
{
    Fixture f;
    f.initV1();
    f.mock.roomTempResult = {2350};

    auto r = f.mgr.getRoomTemperature();
    REQUIRE(r.has_value());
    REQUIRE(*r == 2350);
    REQUIRE(f.mock.getRoomTempCallCount == 1);
    REQUIRE(f.mock.getCoarseCallCount == 0);
}

TEST_CASE("S21Manager getRoomTemperature failure falls back to coarse indoor", "[s21mgr][fallback]")
{
    Fixture f;
    f.initV1();
    f.mock.roomTempResult = tl::unexpected(presError("error"));
    f.mock.coarseResult   = {std::make_tuple<int16_t, int16_t, uint8_t>(2400, 3100, 60)};

    auto r = f.mgr.getRoomTemperature();
    REQUIRE(r.has_value());
    REQUIRE(*r == 2400);
    REQUIRE(f.mock.getCoarseCallCount == 1);
}

TEST_CASE("S21Manager getRoomTemperature: after threshold failures, skips RH fetch",
          "[s21mgr][fallback]")
{
    Fixture f;
    f.initV1();
    f.mock.roomTempResult = tl::unexpected(presError("error"));
    f.mock.coarseResult   = {std::make_tuple<int16_t, int16_t, uint8_t>(2400, 3100, 60)};

    // 4 failures (kThreshold=3, so 4th call marks Unsupported)
    for (int i = 0; i < 4; ++i) {
        f.mgr.getRoomTemperature();
        f.expireCoarseCache(); // expire coarse so each fallback triggers a fresh coarse fetch
    }
    int roomTempCallsBefore = f.mock.getRoomTempCallCount;

    // 5th call: capability is Unsupported, should skip RH entirely
    f.mgr.getRoomTemperature();
    REQUIRE(f.mock.getRoomTempCallCount == roomTempCallsBefore);
}

// ─── Fallback: getOutdoorTemperature ─────────────────────────────────────────

TEST_CASE("S21Manager getOutdoorTemperature returns Ra value on success", "[s21mgr][fallback]")
{
    Fixture f;
    f.initV1();
    f.mock.outdoorTempResult = {3100};

    auto r = f.mgr.getOutdoorTemperature();
    REQUIRE(r.has_value());
    REQUIRE(*r == 3100);
    REQUIRE(f.mock.getOutdoorTempCallCount == 1);
    REQUIRE(f.mock.getCoarseCallCount == 0);
}

TEST_CASE("S21Manager getOutdoorTemperature failure falls back to coarse outdoor",
          "[s21mgr][fallback]")
{
    Fixture f;
    f.initV1();
    f.mock.outdoorTempResult = tl::unexpected(presError("error"));
    f.mock.coarseResult      = {std::make_tuple<int16_t, int16_t, uint8_t>(2400, 3100, 60)};

    auto r = f.mgr.getOutdoorTemperature();
    REQUIRE(r.has_value());
    REQUIRE(*r == 3100);
    REQUIRE(f.mock.getCoarseCallCount == 1);
}

// ─── Fallback: getHumidity ────────────────────────────────────────────────────

TEST_CASE("S21Manager getHumidity returns Re value on success", "[s21mgr][fallback]")
{
    Fixture f;
    f.initV1();
    f.mock.humidityResult = {65};

    auto r = f.mgr.getHumidity();
    REQUIRE(r.has_value());
    REQUIRE(*r == 65);
    REQUIRE(f.mock.getHumidityCallCount == 1);
    REQUIRE(f.mock.getCoarseCallCount == 0);
}

TEST_CASE("S21Manager getHumidity failure falls back to coarse humidity", "[s21mgr][fallback]")
{
    Fixture f;
    f.initV1();
    f.mock.humidityResult = tl::unexpected(presError("error"));
    f.mock.coarseResult   = {std::make_tuple<int16_t, int16_t, uint8_t>(2400, 3100, 60)};

    auto r = f.mgr.getHumidity();
    REQUIRE(r.has_value());
    REQUIRE(*r == 60);
    REQUIRE(f.mock.getCoarseCallCount == 1);
}

TEST_CASE("S21Manager: when coarse itself fails, getRoomTemperature returns error",
          "[s21mgr][fallback]")
{
    Fixture f;
    f.initV1();
    f.mock.roomTempResult = tl::unexpected(presError("RH error"));
    f.mock.coarseResult   = tl::unexpected(presError("F9 error"));

    auto r = f.mgr.getRoomTemperature();
    REQUIRE_FALSE(r.has_value());
    REQUIRE(std::string_view{r.error().message} == "F9 error");
}

TEST_CASE("S21Manager: coarse Unsupported after threshold — no presentation call",
          "[s21mgr][fallback]")
{
    Fixture f;
    f.initV1();
    f.mock.roomTempResult = tl::unexpected(presError("error"));
    f.mock.coarseResult   = tl::unexpected(presError("F9 error"));

    // Drive coarse capability to Unsupported (4 failures via getRoomTemperature fallback)
    for (int i = 0; i < 4; ++i) {
        f.mgr.getRoomTemperature();
        // Expire coarse cache so each call triggers an actual fetch attempt
        f.expireCoarseCache();
    }
    int coarseCallsBefore = f.mock.getCoarseCallCount;

    // Next call: coarse is Unsupported — FetchFn should not call the presentation
    f.expireCoarseCache();
    f.mgr.getRoomTemperature();
    REQUIRE(f.mock.getCoarseCallCount == coarseCallsBefore);
}

// ─── Fallback: getFanMode ─────────────────────────────────────────────────────

TEST_CASE("S21Manager getFanMode returns RG value on success", "[s21mgr][fallback]")
{
    Fixture f;
    f.initV1();
    f.mock.fanModeResult = {FanMode::High};

    auto r = f.mgr.getFanMode();
    REQUIRE(r.has_value());
    REQUIRE(*r == FanMode::High);
    REQUIRE(f.mock.getFanModeCallCount == 1);
    REQUIRE(f.mock.getOperationCallCount == 0);
}

TEST_CASE("S21Manager getFanMode failure falls back to operation cache", "[s21mgr][fallback]")
{
    Fixture f;
    f.initV1();
    f.mock.fanModeResult   = tl::unexpected(presError("error"));
    f.mock.operationResult = {std::make_tuple(true, OperatingMode::Cool, 2200, FanMode::Medium)};

    auto r = f.mgr.getFanMode();
    REQUIRE(r.has_value());
    REQUIRE(*r == FanMode::Medium);
    REQUIRE(f.mock.getOperationCallCount == 1);
}

TEST_CASE("S21Manager getFanMode failure + fresh op cache: no extra getOperation call",
          "[s21mgr][fallback]")
{
    Fixture f;
    f.initV1();

    // Prime the operation cache
    f.mock.operationResult = {std::make_tuple(true, OperatingMode::Cool, 2200, FanMode::Quiet)};
    f.mgr.getOnOff();
    REQUIRE(f.mock.getOperationCallCount == 1);

    // getFanMode fails → uses fresh cached fan mode
    f.mock.fanModeResult = tl::unexpected(presError("error"));
    auto r = f.mgr.getFanMode();
    REQUIRE(r.has_value());
    REQUIRE(*r == FanMode::Quiet);
    REQUIRE(f.mock.getOperationCallCount == 1); // no extra fetch
}

TEST_CASE("S21Manager getFanMode failure + stale op cache + op fetch failure: error returned",
          "[s21mgr][fallback]")
{
    Fixture f;
    f.initV1();

    f.mock.fanModeResult   = tl::unexpected(presError("error"));
    f.mock.operationResult = tl::unexpected(presError("timeout"));

    auto r = f.mgr.getFanMode();
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("S21Manager getFanMode Unsupported: skips RG entirely, goes to op cache",
          "[s21mgr][fallback]")
{
    Fixture f;
    f.initV1();
    f.mock.operationResult = {std::make_tuple(true, OperatingMode::Cool, 2200, FanMode::MidLow)};
    f.mock.fanModeResult   = tl::unexpected(presError("error"));

    // Drive to Unsupported
    for (int i = 0; i < 4; ++i) {
        f.mgr.getFanMode();
    }
    int fanCallsBefore = f.mock.getFanModeCallCount;

    // 5th call: Unsupported — skips RG
    auto r = f.mgr.getFanMode();
    REQUIRE(r.has_value());
    REQUIRE(*r == FanMode::MidLow);
    REQUIRE(f.mock.getFanModeCallCount == fanCallsBefore);
}

// ─── Setters ──────────────────────────────────────────────────────────────────

TEST_CASE("S21Manager setOperation sends correct arguments", "[s21mgr][set]")
{
    Fixture f;
    f.initV1();

    f.mgr.setOperation(true, OperatingMode::Heat, 2500, FanMode::Low);

    REQUIRE(f.mock.setOperationCallCount == 1);
    REQUIRE(f.mock.lastOnOff == true);
    REQUIRE(f.mock.lastMode == OperatingMode::Heat);
    REQUIRE(f.mock.lastSetPoint == 2500);
    REQUIRE(f.mock.lastFanMode == FanMode::Low);
}

TEST_CASE("S21Manager setOperation success: getOnOff returns new value without re-fetch",
          "[s21mgr][set]")
{
    Fixture f;
    f.initV1();

    f.mock.operationResult = {std::make_tuple(false, OperatingMode::Cool, 2200, FanMode::Auto)};
    f.mgr.getOnOff(); // prime cache with off
    REQUIRE(f.mock.getOperationCallCount == 1);

    f.mgr.setOperation(true, OperatingMode::Cool, 2200, FanMode::Auto);

    auto r = f.mgr.getOnOff();
    REQUIRE(*r == true);
    REQUIRE(f.mock.getOperationCallCount == 1); // no new fetch
}

TEST_CASE("S21Manager setOperation failure: error returned, cache not updated",
          "[s21mgr][set]")
{
    Fixture f;
    f.initV1();

    f.mock.operationResult    = {std::make_tuple(false, OperatingMode::Cool, 2200, FanMode::Auto)};
    f.mock.setOperationResult = tl::unexpected(dlError("NAK"));

    auto r = f.mgr.setOperation(true, OperatingMode::Cool, 2200, FanMode::Auto);
    REQUIRE_FALSE(r.has_value());

    // Next getOnOff should re-fetch (cache was not updated by failed setOperation)
    f.expireOperationCache();
    f.mgr.getOnOff();
    REQUIRE(f.mock.getOperationCallCount == 1);
}

TEST_CASE("S21Manager setOnOff with fresh cache sends correct setOperation", "[s21mgr][set]")
{
    Fixture f;
    f.initV1();

    // Prime cache
    f.mock.operationResult = {std::make_tuple(false, OperatingMode::Heat, 2500, FanMode::Low)};
    f.mgr.getOnOff();
    REQUIRE(f.mock.getOperationCallCount == 1);

    f.mgr.setOnOff(true);

    REQUIRE(f.mock.setOperationCallCount == 1);
    REQUIRE(f.mock.lastOnOff == true);
    REQUIRE(f.mock.lastMode == OperatingMode::Heat);  // cached value preserved
    REQUIRE(f.mock.lastSetPoint == 2500);              // cached value preserved
    REQUIRE(f.mock.lastFanMode == FanMode::Low);       // cached value preserved
    REQUIRE(f.mock.getOperationCallCount == 1);        // no extra fetch
}

TEST_CASE("S21Manager setOnOff with stale cache: re-fetches then sets", "[s21mgr][set]")
{
    Fixture f;
    f.initV1();

    f.mock.operationResult = {std::make_tuple(false, OperatingMode::Cool, 2200, FanMode::Auto)};
    // Don't prime cache — it's empty (stale)

    f.mgr.setOnOff(true);

    REQUIRE(f.mock.getOperationCallCount == 1); // fetched for cache
    REQUIRE(f.mock.setOperationCallCount == 1);
    REQUIRE(f.mock.lastOnOff == true);
    REQUIRE(f.mock.lastMode == OperatingMode::Cool);
}

TEST_CASE("S21Manager setOnOff with stale cache + getOperation failure: error, no setOperation",
          "[s21mgr][set]")
{
    Fixture f;
    f.initV1();

    f.mock.operationResult = tl::unexpected(presError("timeout"));

    auto r = f.mgr.setOnOff(true);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(f.mock.setOperationCallCount == 0);
}

TEST_CASE("S21Manager setSetpoint with fresh cache sends correct setOperation", "[s21mgr][set]")
{
    Fixture f;
    f.initV1();

    f.mock.operationResult = {std::make_tuple(true, OperatingMode::Cool, 2200, FanMode::Auto)};
    f.mgr.getOnOff();

    f.mgr.setSetpoint(2600);

    REQUIRE(f.mock.setOperationCallCount == 1);
    REQUIRE(f.mock.lastOnOff == true);
    REQUIRE(f.mock.lastSetPoint == 2600);  // new setpoint
    REQUIRE(f.mock.lastMode == OperatingMode::Cool);
    REQUIRE(f.mock.lastFanMode == FanMode::Auto);
}

TEST_CASE("S21Manager setFanMode with fresh cache sends correct setOperation", "[s21mgr][set]")
{
    Fixture f;
    f.initV1();

    f.mock.operationResult = {std::make_tuple(true, OperatingMode::Cool, 2200, FanMode::Auto)};
    f.mgr.getOnOff();

    f.mgr.setFanMode(FanMode::High);

    REQUIRE(f.mock.setOperationCallCount == 1);
    REQUIRE(f.mock.lastFanMode == FanMode::High);  // new fan mode
    REQUIRE(f.mock.lastOnOff == true);
    REQUIRE(f.mock.lastSetPoint == 2200);
}

TEST_CASE("S21Manager setOperation updates cache: subsequent setOnOff uses updated cache",
          "[s21mgr][set]")
{
    Fixture f;
    f.initV1();

    // Initial state from presentation: off
    f.mock.operationResult = {std::make_tuple(false, OperatingMode::Cool, 2200, FanMode::Auto)};

    // Set to on via setOperation → cache updated to on
    f.mgr.setOperation(true, OperatingMode::Cool, 2200, FanMode::Auto);
    REQUIRE(f.mock.getOperationCallCount == 0); // setOperation didn't need to fetch

    // setOnOff(false) should use cached (on) state, then set off
    f.mgr.setOnOff(false);
    REQUIRE(f.mock.getOperationCallCount == 0); // still no getOperation — cache is fresh
    REQUIRE(f.mock.lastOnOff == false);
    REQUIRE(f.mock.lastMode == OperatingMode::Cool); // from setOperation cache
}

// ─── Error messages ───────────────────────────────────────────────────────────

TEST_CASE("S21Manager PresentationError message preserved in Error", "[s21mgr][errors]")
{
    Fixture f;
    f.initV1();
    f.mock.operationResult = tl::unexpected(presError("unexpected response code"));

    auto r = f.mgr.getOnOff();
    REQUIRE_FALSE(r.has_value());
    REQUIRE(std::string_view{r.error().message} == "unexpected response code");
}

TEST_CASE("S21Manager DataLinkError message preserved in Error from setOperation",
          "[s21mgr][errors]")
{
    Fixture f;
    f.initV1();
    f.mock.setOperationResult = tl::unexpected(dlError("NAK"));

    auto r = f.mgr.setOperation(true, OperatingMode::Cool, 2200, FanMode::Auto);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(std::string_view{r.error().message} == "NAK");
}

TEST_CASE("S21Manager not-ready error text is 'not ready'", "[s21mgr][errors]")
{
    Fixture f;
    // Do not call Init()
    auto r = f.mgr.getOnOff();
    REQUIRE_FALSE(r.has_value());
    REQUIRE(std::string_view{r.error().message} == "not ready");
}
