/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#include <catch2/catch_test_macros.hpp>

#include "operational_mode.h"

#include <type_traits>
#include <vector>

using sync::OperationalMode;
using sync::RunningMode;

TEST_CASE("OperationalMode is a uint8_t-sized enum class", "[operational_mode]")
{
    STATIC_REQUIRE(std::is_enum_v<OperationalMode>);
    STATIC_REQUIRE(sizeof(OperationalMode) == 1);
    STATIC_REQUIRE(std::is_same_v<std::underlying_type_t<OperationalMode>, uint8_t>);
}

TEST_CASE("OperationalMode enumerators are pairwise distinct", "[operational_mode]")
{
    const std::vector<OperationalMode> all = {
        OperationalMode::Auto,
        OperationalMode::Cool,
        OperationalMode::Heat,
        OperationalMode::Dry,
        OperationalMode::FanOnly,
    };
    for (size_t i = 0; i < all.size(); ++i) {
        for (size_t j = i + 1; j < all.size(); ++j) {
            REQUIRE(all[i] != all[j]);
        }
    }
}

TEST_CASE("RunningMode is a uint8_t-sized enum class", "[running_mode]")
{
    STATIC_REQUIRE(std::is_enum_v<RunningMode>);
    STATIC_REQUIRE(sizeof(RunningMode) == 1);
    STATIC_REQUIRE(std::is_same_v<std::underlying_type_t<RunningMode>, uint8_t>);
}

TEST_CASE("RunningMode enumerators are pairwise distinct", "[running_mode]")
{
    const std::vector<RunningMode> all = {
        RunningMode::Off,
        RunningMode::Cooling,
        RunningMode::Heating,
    };
    for (size_t i = 0; i < all.size(); ++i) {
        for (size_t j = i + 1; j < all.size(); ++j) {
            REQUIRE(all[i] != all[j]);
        }
    }
}
