/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Tests for fan_mapping.h — the pure speed↔percent↔mode conversions that
 * implement the Matter FanControl Percent/Speed Rules (§4.4.6.3.1 /
 * §4.4.6.6.1) for SpeedMax = 6.
 */

#include <catch2/catch_test_macros.hpp>

#include "fan_mapping.h"

using namespace sync;

TEST_CASE("speedLevelToPercent: floor(level/6·100) per the Speed Rules table",
          "[fan_mapping]")
{
    REQUIRE(speedLevelToPercent(FanLevel::Quiet)   == 16);  // 1
    REQUIRE(speedLevelToPercent(FanLevel::Low)     == 33);  // 2
    REQUIRE(speedLevelToPercent(FanLevel::MidLow)  == 50);  // 3
    REQUIRE(speedLevelToPercent(FanLevel::Medium)  == 66);  // 4
    REQUIRE(speedLevelToPercent(FanLevel::MidHigh) == 83);  // 5
    REQUIRE(speedLevelToPercent(FanLevel::High)    == 100); // 6
}

TEST_CASE("percentToSpeedLevel: ceil(6·percent·0.01), clamped to 1..6",
          "[fan_mapping]")
{
    // The canonical lossy example from the plan: 40% → speed 3.
    REQUIRE(percentToSpeedLevel(40) == FanLevel::MidLow);

    // Range boundaries (Percent Rules: 1-33→Low(1-2), 34-66→Med(3-4),
    // 67-100→High(5-6)).
    REQUIRE(percentToSpeedLevel(1)   == FanLevel::Quiet);   // 1
    REQUIRE(percentToSpeedLevel(33)  == FanLevel::Low);     // 2
    REQUIRE(percentToSpeedLevel(34)  == FanLevel::MidLow);  // 3
    REQUIRE(percentToSpeedLevel(66)  == FanLevel::Medium);  // 4
    REQUIRE(percentToSpeedLevel(67)  == FanLevel::MidHigh); // 5
    REQUIRE(percentToSpeedLevel(100) == FanLevel::High);    // 6
}

TEST_CASE("speed↔percent ranges stay inside one FanMode range (no cross-range drift)",
          "[fan_mapping]")
{
    // A percent write maps to a level whose floor-percent lands in the same
    // FanMode range, so the trio (FanMode/Speed/Percent) is self-consistent.
    for (uint8_t p = 1; p <= 100; ++p) {
        const FanLevel lvl = percentToSpeedLevel(p);
        REQUIRE(fanModeOf(FanSpeed{lvl}) ==
                fanModeOf(FanSpeed{percentToSpeedLevel(speedLevelToPercent(lvl))}));
    }
}

TEST_CASE("fanModeOf: null→Auto, 1-2→Low, 3-4→Medium, 5-6→High",
          "[fan_mapping]")
{
    REQUIRE(fanModeOf(std::nullopt)            == FanModeCategory::Auto);
    REQUIRE(fanModeOf(FanSpeed{FanLevel::Quiet})   == FanModeCategory::Low);
    REQUIRE(fanModeOf(FanSpeed{FanLevel::Low})     == FanModeCategory::Low);
    REQUIRE(fanModeOf(FanSpeed{FanLevel::MidLow})  == FanModeCategory::Medium);
    REQUIRE(fanModeOf(FanSpeed{FanLevel::Medium})  == FanModeCategory::Medium);
    REQUIRE(fanModeOf(FanSpeed{FanLevel::MidHigh}) == FanModeCategory::High);
    REQUIRE(fanModeOf(FanSpeed{FanLevel::High})    == FanModeCategory::High);
}

TEST_CASE("representativeSpeed: Low/Medium/High → 2/4/6", "[fan_mapping]")
{
    REQUIRE(representativeSpeed(FanModeCategory::Low)    == FanLevel::Low);    // 2
    REQUIRE(representativeSpeed(FanModeCategory::Medium) == FanLevel::Medium); // 4
    REQUIRE(representativeSpeed(FanModeCategory::High)   == FanLevel::High);   // 6
}
