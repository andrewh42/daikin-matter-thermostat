/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#include <catch2/catch_test_macros.hpp>

#include "logical_attribute.h"

#include <type_traits>
#include <vector>

using sync::LogicalAttribute;

TEST_CASE("LogicalAttribute is a uint8_t-sized enum class", "[logical_attribute]")
{
    STATIC_REQUIRE(std::is_enum_v<LogicalAttribute>);
    STATIC_REQUIRE(sizeof(LogicalAttribute) == 1);
    STATIC_REQUIRE(std::is_same_v<std::underlying_type_t<LogicalAttribute>, uint8_t>);
}

TEST_CASE("LogicalAttribute enumerators are pairwise distinct", "[logical_attribute]")
{
    const std::vector<LogicalAttribute> all = {
        LogicalAttribute::OnOff,
        LogicalAttribute::SystemMode,
        LogicalAttribute::OccupiedHeatingSetpoint,
        LogicalAttribute::OccupiedCoolingSetpoint,
        LogicalAttribute::RunningMode,
        LogicalAttribute::LocalTemperature,
        LogicalAttribute::OutdoorTemperature,
        LogicalAttribute::SetpointSource,
        LogicalAttribute::SpeedSetting,
        LogicalAttribute::FanMode,
        LogicalAttribute::SpeedCurrent,
        LogicalAttribute::PercentSetting,
        LogicalAttribute::PercentCurrent,
        LogicalAttribute::Humidity,
        LogicalAttribute::Reachable,
    };

    for (size_t i = 0; i < all.size(); ++i) {
        for (size_t j = i + 1; j < all.size(); ++j) {
            REQUIRE(all[i] != all[j]);
        }
    }
}

TEST_CASE("LogicalAttribute supports equality and inequality", "[logical_attribute]")
{
    REQUIRE(LogicalAttribute::OnOff == LogicalAttribute::OnOff);
    REQUIRE(LogicalAttribute::OnOff != LogicalAttribute::SystemMode);
}
