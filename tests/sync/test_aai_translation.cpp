/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Tests for aai_translation.h — the pure-function translation tables
 * between sync/ domain types and CHIP cluster types. Every domain enum
 * exercised, plus the round-trip identity where defined.
 */

#include <catch2/catch_test_macros.hpp>

#include "aai_translation.h"
#include "logical_attribute.h"
#include "operational_mode.h"

using sync::LogicalAttribute;
using sync::OperationalMode;
using sync::RunningMode;
using sync::ObservationSource;
using sync::FanLevel;
using sync::FanSpeed;
using sync::FanModeCategory;

using SystemModeEnum            = chip::app::Clusters::Thermostat::SystemModeEnum;
using ThermostatRunningModeEnum = chip::app::Clusters::Thermostat::ThermostatRunningModeEnum;
using SetpointChangeSourceEnum  = chip::app::Clusters::Thermostat::SetpointChangeSourceEnum;
using FanModeEnum               = chip::app::Clusters::FanControl::FanModeEnum;

// ─── SystemMode ──────────────────────────────────────────────────────────────

TEST_CASE("toMatterSystemMode: !onOff overrides operational mode → kOff",
          "[aai_translation][system_mode]")
{
    REQUIRE(sync_aai::toMatterSystemMode(false, OperationalMode::Auto)
            == SystemModeEnum::kOff);
    REQUIRE(sync_aai::toMatterSystemMode(false, OperationalMode::Cool)
            == SystemModeEnum::kOff);
    REQUIRE(sync_aai::toMatterSystemMode(false, OperationalMode::Heat)
            == SystemModeEnum::kOff);
}

TEST_CASE("toMatterSystemMode: onOff=true maps each OperationalMode 1:1",
          "[aai_translation][system_mode]")
{
    REQUIRE(sync_aai::toMatterSystemMode(true, OperationalMode::Auto)
            == SystemModeEnum::kAuto);
    REQUIRE(sync_aai::toMatterSystemMode(true, OperationalMode::Cool)
            == SystemModeEnum::kCool);
    REQUIRE(sync_aai::toMatterSystemMode(true, OperationalMode::Heat)
            == SystemModeEnum::kHeat);
    REQUIRE(sync_aai::toMatterSystemMode(true, OperationalMode::Dry)
            == SystemModeEnum::kDry);
    REQUIRE(sync_aai::toMatterSystemMode(true, OperationalMode::FanOnly)
            == SystemModeEnum::kFanOnly);
}

TEST_CASE("fromMatterSystemMode: kOff → (false, retained mode placeholder Auto)",
          "[aai_translation][system_mode]")
{
    auto p = sync_aai::fromMatterSystemMode(SystemModeEnum::kOff);
    REQUIRE(p.has_value());
    REQUIRE(p->first == false);
    REQUIRE(p->second == OperationalMode::Auto);
}

TEST_CASE("fromMatterSystemMode: representable modes map 1:1 with power=true",
          "[aai_translation][system_mode]")
{
    auto autoP = sync_aai::fromMatterSystemMode(SystemModeEnum::kAuto);
    REQUIRE(autoP.has_value());
    REQUIRE(*autoP == std::pair{true, OperationalMode::Auto});

    auto coolP = sync_aai::fromMatterSystemMode(SystemModeEnum::kCool);
    REQUIRE(coolP.has_value());
    REQUIRE(*coolP == std::pair{true, OperationalMode::Cool});

    auto heatP = sync_aai::fromMatterSystemMode(SystemModeEnum::kHeat);
    REQUIRE(heatP.has_value());
    REQUIRE(*heatP == std::pair{true, OperationalMode::Heat});

    auto dryP = sync_aai::fromMatterSystemMode(SystemModeEnum::kDry);
    REQUIRE(dryP.has_value());
    REQUIRE(*dryP == std::pair{true, OperationalMode::Dry});

    auto fanP = sync_aai::fromMatterSystemMode(SystemModeEnum::kFanOnly);
    REQUIRE(fanP.has_value());
    REQUIRE(*fanP == std::pair{true, OperationalMode::FanOnly});
}

TEST_CASE("fromMatterSystemMode: EmergencyHeat / Precooling collapse to nearest equivalent",
          "[aai_translation][system_mode]")
{
    auto eh = sync_aai::fromMatterSystemMode(SystemModeEnum::kEmergencyHeat);
    REQUIRE(eh.has_value());
    REQUIRE(*eh == std::pair{true, OperationalMode::Heat});

    auto pc = sync_aai::fromMatterSystemMode(SystemModeEnum::kPrecooling);
    REQUIRE(pc.has_value());
    REQUIRE(*pc == std::pair{true, OperationalMode::Cool});
}

TEST_CASE("fromMatterSystemMode: kSleep is rejected (nullopt)",
          "[aai_translation][system_mode]")
{
    REQUIRE_FALSE(sync_aai::fromMatterSystemMode(SystemModeEnum::kSleep).has_value());
}

TEST_CASE("fromMatterSystemMode: round-trip identity for representable modes",
          "[aai_translation][system_mode]")
{
    const std::vector<std::pair<bool, OperationalMode>> roundtrips = {
        {false, OperationalMode::Auto}, // SystemMode kOff → (false, Auto)
        {true,  OperationalMode::Auto},
        {true,  OperationalMode::Cool},
        {true,  OperationalMode::Heat},
        {true,  OperationalMode::Dry},
        {true,  OperationalMode::FanOnly},
    };
    for (const auto& [power, mode] : roundtrips) {
        const auto matter = sync_aai::toMatterSystemMode(power, mode);
        const auto back   = sync_aai::fromMatterSystemMode(matter);
        REQUIRE(back.has_value());
        REQUIRE(*back == std::pair{power, mode});
    }
}

// ─── RunningMode ─────────────────────────────────────────────────────────────

TEST_CASE("toMatterRunningMode covers every domain value",
          "[aai_translation][running_mode]")
{
    REQUIRE(sync_aai::toMatterRunningMode(RunningMode::Off)
            == ThermostatRunningModeEnum::kOff);
    REQUIRE(sync_aai::toMatterRunningMode(RunningMode::Cooling)
            == ThermostatRunningModeEnum::kCool);
    REQUIRE(sync_aai::toMatterRunningMode(RunningMode::Heating)
            == ThermostatRunningModeEnum::kHeat);
}

// ─── SetpointSource ──────────────────────────────────────────────────────────

TEST_CASE("toMatterSetpointSource maps ObservationSource to spec values",
          "[aai_translation][setpoint_source]")
{
    REQUIRE(sync_aai::toMatterSetpointSource(ObservationSource::Matter)
            == SetpointChangeSourceEnum::kExternal);
    REQUIRE(sync_aai::toMatterSetpointSource(ObservationSource::Device)
            == SetpointChangeSourceEnum::kManual);
    REQUIRE(sync_aai::toMatterSetpointSource(ObservationSource::Boot)
            == SetpointChangeSourceEnum::kManual);
}

// ─── FanMode ─────────────────────────────────────────────────────────────────

TEST_CASE("toMatterFanMode: FanModeCategory maps to the cluster FanMode",
          "[aai_translation][fan_mode]")
{
    REQUIRE(sync_aai::toMatterFanMode(FanModeCategory::Off)    == FanModeEnum::kOff);
    REQUIRE(sync_aai::toMatterFanMode(FanModeCategory::Low)    == FanModeEnum::kLow);
    REQUIRE(sync_aai::toMatterFanMode(FanModeCategory::Medium) == FanModeEnum::kMedium);
    REQUIRE(sync_aai::toMatterFanMode(FanModeCategory::High)   == FanModeEnum::kHigh);
    REQUIRE(sync_aai::toMatterFanMode(FanModeCategory::Auto)   == FanModeEnum::kAuto);
}

TEST_CASE("fromMatterFanMode: Off → power off; Auto/Smart → Auto (null speed)",
          "[aai_translation][fan_mode]")
{
    REQUIRE(sync_aai::fromMatterFanMode(FanModeEnum::kOff).kind
            == sync_aai::FanModeWriteKind::PowerOff);

    auto autoF = sync_aai::fromMatterFanMode(FanModeEnum::kAuto);
    auto smart = sync_aai::fromMatterFanMode(FanModeEnum::kSmart);
    REQUIRE(autoF.kind == sync_aai::FanModeWriteKind::SetSpeed);
    REQUIRE_FALSE(autoF.speed.has_value());
    REQUIRE(smart.kind == sync_aai::FanModeWriteKind::SetSpeed);
    REQUIRE_FALSE(smart.speed.has_value());
}

TEST_CASE("fromMatterFanMode: Low/Medium/High → representative levels 2/4/6",
          "[aai_translation][fan_mode]")
{
    auto low  = sync_aai::fromMatterFanMode(FanModeEnum::kLow);
    auto med  = sync_aai::fromMatterFanMode(FanModeEnum::kMedium);
    auto high = sync_aai::fromMatterFanMode(FanModeEnum::kHigh);
    REQUIRE(low.kind  == sync_aai::FanModeWriteKind::SetSpeed);
    REQUIRE(low.speed  == FanSpeed{FanLevel::Low});    // 2
    REQUIRE(med.speed  == FanSpeed{FanLevel::Medium}); // 4
    REQUIRE(high.speed == FanSpeed{FanLevel::High});   // 6
}

TEST_CASE("fromMatterFanMode: kOn → High (§4.4.6.1.3)",
          "[aai_translation][fan_mode]")
{
    auto on = sync_aai::fromMatterFanMode(FanModeEnum::kOn);
    REQUIRE(on.kind  == sync_aai::FanModeWriteKind::SetSpeed);
    REQUIRE(on.speed == FanSpeed{FanLevel::High});
}

// ─── LogicalAttribute → (cluster, attribute) ─────────────────────────────────

TEST_CASE("toMatterAddress maps every LogicalAttribute (except Reachable) to a real cluster path",
          "[aai_translation][logical_attribute]")
{
    namespace Cl     = chip::app::Clusters;
    namespace TAttr  = Cl::Thermostat::Attributes;
    namespace FCAttr = Cl::FanControl::Attributes;
    namespace OOAttr = Cl::OnOff::Attributes;
    namespace RHAttr = Cl::RelativeHumidityMeasurement::Attributes;

    REQUIRE(sync_aai::toMatterAddress(LogicalAttribute::OnOff)
            == std::pair{Cl::OnOff::Id, OOAttr::OnOff::Id});
    REQUIRE(sync_aai::toMatterAddress(LogicalAttribute::SystemMode)
            == std::pair{Cl::Thermostat::Id, TAttr::SystemMode::Id});
    REQUIRE(sync_aai::toMatterAddress(LogicalAttribute::OccupiedHeatingSetpoint)
            == std::pair{Cl::Thermostat::Id, TAttr::OccupiedHeatingSetpoint::Id});
    REQUIRE(sync_aai::toMatterAddress(LogicalAttribute::OccupiedCoolingSetpoint)
            == std::pair{Cl::Thermostat::Id, TAttr::OccupiedCoolingSetpoint::Id});
    REQUIRE(sync_aai::toMatterAddress(LogicalAttribute::RunningMode)
            == std::pair{Cl::Thermostat::Id, TAttr::ThermostatRunningMode::Id});
    REQUIRE(sync_aai::toMatterAddress(LogicalAttribute::LocalTemperature)
            == std::pair{Cl::Thermostat::Id, TAttr::LocalTemperature::Id});
    REQUIRE(sync_aai::toMatterAddress(LogicalAttribute::OutdoorTemperature)
            == std::pair{Cl::Thermostat::Id, TAttr::OutdoorTemperature::Id});
    REQUIRE(sync_aai::toMatterAddress(LogicalAttribute::SetpointSource)
            == std::pair{Cl::Thermostat::Id, TAttr::SetpointChangeSource::Id});
    REQUIRE(sync_aai::toMatterAddress(LogicalAttribute::SpeedSetting)
            == std::pair{Cl::FanControl::Id, FCAttr::SpeedSetting::Id});
    REQUIRE(sync_aai::toMatterAddress(LogicalAttribute::FanMode)
            == std::pair{Cl::FanControl::Id, FCAttr::FanMode::Id});
    REQUIRE(sync_aai::toMatterAddress(LogicalAttribute::SpeedCurrent)
            == std::pair{Cl::FanControl::Id, FCAttr::SpeedCurrent::Id});
    REQUIRE(sync_aai::toMatterAddress(LogicalAttribute::PercentSetting)
            == std::pair{Cl::FanControl::Id, FCAttr::PercentSetting::Id});
    REQUIRE(sync_aai::toMatterAddress(LogicalAttribute::PercentCurrent)
            == std::pair{Cl::FanControl::Id, FCAttr::PercentCurrent::Id});
    REQUIRE(sync_aai::toMatterAddress(LogicalAttribute::Humidity)
            == std::pair{Cl::RelativeHumidityMeasurement::Id, RHAttr::MeasuredValue::Id});
}

TEST_CASE("toMatterAddress: Reachable returns the {0,0} sentinel",
          "[aai_translation][logical_attribute]")
{
    REQUIRE(sync_aai::toMatterAddress(LogicalAttribute::Reachable)
            == std::pair<chip::ClusterId, chip::AttributeId>{0, 0});
}

// ─── wrap (optional ↔ Nullable) ──────────────────────────────────────────────

TEST_CASE("wrap: nullopt → Nullable::Null", "[aai_translation][wrap]")
{
    std::optional<int16_t> v;
    auto n = sync_aai::wrap(v);
    REQUIRE(n.IsNull());
}

TEST_CASE("wrap: present value → Nullable::NonNull with same value",
          "[aai_translation][wrap]")
{
    std::optional<int16_t> v = 2350;
    auto n = sync_aai::wrap(v);
    REQUIRE_FALSE(n.IsNull());
    REQUIRE(n.Value() == 2350);
}

TEST_CASE("wrap: zero is not confused with null", "[aai_translation][wrap]")
{
    std::optional<int16_t> v = 0;
    auto n = sync_aai::wrap(v);
    REQUIRE_FALSE(n.IsNull());
    REQUIRE(n.Value() == 0);
}
