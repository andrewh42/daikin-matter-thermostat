/*
 * Phase 0 smoke test: confirm the sync_tests target builds and ctest discovers
 * tests. Replaced by real coverage in Phases 1–4.
 *
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#include <catch2/catch_test_macros.hpp>

#include <app-common/zap-generated/attributes/Accessors.h>

TEST_CASE("sync_tests target wiring", "[phase0]")
{
    REQUIRE(chip::app::Clusters::Thermostat::Id == 0x0201);
    REQUIRE(chip::app::Clusters::OnOff::Id      == 0x0006);
    REQUIRE(chip::app::Clusters::FanControl::Id == 0x0202);
}
