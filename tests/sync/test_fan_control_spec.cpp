/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * End-to-end FanControl spec-compliance tests through the Reconciler:
 *   - null SpeedSetting/PercentSetting writes are no-ops
 *   - Off (speed/percent 0) couples to AC power-off
 *   - a non-off fan write while powered off turns the AC on and restores
 *     SystemMode to the retained mode
 *   - the remembered exact PercentSetting survives a confirming echo but
 *     re-derives on a genuine device speed change
 *   - FanMode reflects the speed range; the trio stays self-consistent
 */

#include <catch2/catch_test_macros.hpp>

#include "fan_mapping.h"
#include "reconciler.h"

#include <algorithm>

using namespace sync;

namespace {

struct Fixture {
    ManualTimeSource time;
    LogicalACState   state;
    Reconciler       rec;
    explicit Fixture(LogicalACStateDefaults d) : time(0), state(d), rec(state, time) {}
    const Projector& p() const { return rec.projector(); }
};

LogicalACStateDefaults onCool(int16_t coolSp = 2400)
{
    LogicalACStateDefaults d;
    d.onOff        = true;
    d.mode         = OperationalMode::Cool;
    d.coolSetpoint = coolSp;
    return d;
}

bool has(const std::vector<LogicalAttribute>& v, LogicalAttribute a)
{
    return std::any_of(v.begin(), v.end(), [&](LogicalAttribute x) { return x == a; });
}

S21OperationalObservation obs(FanMode fan, int16_t coolSp = 2400)
{
    return {true, OperatingMode::Cool, coolSp, fan, std::nullopt};
}

} // namespace

TEST_CASE("PercentSetting null write is a no-op (§4.4.6.3)", "[fan_spec]")
{
    Fixture f(onCool());
    f.rec.applyIntent(SetSpeedSettingIntent{FanSpeed{FanLevel::Medium}}); // speed 4
    REQUIRE(f.p().projectedSpeedSetting(f.state)   == std::optional<uint8_t>{4});
    REQUIRE(f.p().projectedPercentSetting(f.state) == std::optional<uint8_t>{66});

    auto ch = f.rec.applyIntent(SetPercentSettingIntent{std::nullopt});
    REQUIRE(ch.dirtyAttributes.empty());
    REQUIRE(f.p().projectedSpeedSetting(f.state)   == std::optional<uint8_t>{4});
    REQUIRE(f.p().projectedPercentSetting(f.state) == std::optional<uint8_t>{66});
}

TEST_CASE("Off (speed/percent 0) couples to power-off; fan attributes read 0/Off",
          "[fan_spec]")
{
    Fixture f(onCool());
    f.rec.applyIntent(SetSpeedSettingIntent{FanSpeed{FanLevel::Medium}});

    // The AAI maps SpeedSetting=0 / PercentSetting=0 / FanMode=Off to this.
    auto ch = f.rec.applyIntent(SetOnOffIntent{false});

    REQUIRE(f.p().projectedOnOff(f.state)          == false);
    REQUIRE(f.p().projectedFanMode(f.state)        == FanModeCategory::Off);
    REQUIRE(f.p().projectedSpeedSetting(f.state)   == std::optional<uint8_t>{0});
    REQUIRE(f.p().projectedPercentSetting(f.state) == std::optional<uint8_t>{0});
    REQUIRE(f.p().projectedSpeedCurrent(f.state)   == 0);
    REQUIRE(f.p().projectedPercentCurrent(f.state) == 0);

    REQUIRE(has(ch.dirtyAttributes, LogicalAttribute::OnOff));
    REQUIRE(has(ch.dirtyAttributes, LogicalAttribute::SystemMode));
    REQUIRE(has(ch.dirtyAttributes, LogicalAttribute::FanMode));
    REQUIRE(has(ch.dirtyAttributes, LogicalAttribute::SpeedSetting));
    REQUIRE(has(ch.dirtyAttributes, LogicalAttribute::PercentSetting));
}

TEST_CASE("Non-off fan write while powered off turns the AC on and restores SystemMode",
          "[fan_spec]")
{
    LogicalACStateDefaults d;
    d.onOff        = false;
    d.mode         = OperationalMode::Cool; // retained mode shadow
    d.coolSetpoint = 2400;
    Fixture f(d);

    REQUIRE(f.p().projectedOnOff(f.state)   == false);
    REQUIRE(f.p().projectedFanMode(f.state) == FanModeCategory::Off);

    auto ch = f.rec.applyIntent(SetSpeedSettingIntent{FanSpeed{FanLevel::Medium}});

    REQUIRE(f.p().projectedOnOff(f.state)        == true);
    REQUIRE(f.p().projectedMode(f.state)         == OperationalMode::Cool); // SystemMode restored
    REQUIRE(f.p().projectedFanMode(f.state)      == FanModeCategory::Medium);
    REQUIRE(f.p().projectedSpeedSetting(f.state) == std::optional<uint8_t>{4});

    REQUIRE(has(ch.dirtyAttributes, LogicalAttribute::OnOff));
    REQUIRE(has(ch.dirtyAttributes, LogicalAttribute::SystemMode));
    REQUIRE(has(ch.dirtyAttributes, LogicalAttribute::FanMode));

    // A single S21 command turns the unit on with the restored mode and fan.
    REQUIRE(ch.sendCommand.has_value());
    REQUIRE(ch.sendCommand->onOff         == true);
    REQUIRE(ch.sendCommand->operatingMode == OperatingMode::Cool);
    REQUIRE(ch.sendCommand->fanMode       == FanMode::Medium);
}

TEST_CASE("Remembered percent: exact on write, survives a confirming echo",
          "[fan_spec]")
{
    Fixture f(onCool());
    auto ch = f.rec.applyIntent(SetPercentSettingIntent{40});
    REQUIRE(f.p().projectedPercentSetting(f.state) == std::optional<uint8_t>{40}); // exact
    REQUIRE(f.p().projectedSpeedSetting(f.state)   == std::optional<uint8_t>{3});  // ceil(6·0.40)
    REQUIRE(ch.sendCommand.has_value());

    f.rec.onCommandSent(*ch.sendCommand);
    // Device confirms exactly the speed we asked for (MidLow = 3).
    f.rec.applyOperationalObservation(obs(FanMode::MidLow));

    REQUIRE(f.p().projectedPercentSetting(f.state) == std::optional<uint8_t>{40}); // not 50
    REQUIRE(f.p().projectedSpeedSetting(f.state)   == std::optional<uint8_t>{3});
}

TEST_CASE("Remembered percent re-derives on a genuine device speed change",
          "[fan_spec]")
{
    Fixture f(onCool());
    f.rec.applyOperationalObservation(obs(FanMode::Low));  // device at Low(2)
    REQUIRE(f.p().projectedPercentSetting(f.state) == std::optional<uint8_t>{33});

    f.rec.applyOperationalObservation(obs(FanMode::High)); // external change to High(6)
    REQUIRE(f.p().projectedPercentSetting(f.state) == std::optional<uint8_t>{100});
    REQUIRE(f.p().projectedSpeedSetting(f.state)   == std::optional<uint8_t>{6});

    f.rec.applyOperationalObservation(obs(FanMode::Auto)); // → Auto: PercentSetting null
    REQUIRE_FALSE(f.p().projectedPercentSetting(f.state).has_value());
    REQUIRE(f.p().projectedFanMode(f.state) == FanModeCategory::Auto);
}

TEST_CASE("PercentCurrent tracks the current speed in manual and Auto", "[fan_spec]")
{
    Fixture f(onCool());
    f.rec.applyOperationalObservation(obs(FanMode::Medium));
    REQUIRE(f.p().projectedSpeedCurrent(f.state)   == 4);
    REQUIRE(f.p().projectedPercentCurrent(f.state) == 66);

    f.rec.applyOperationalObservation(obs(FanMode::Auto));
    REQUIRE(f.p().projectedSpeedCurrent(f.state)   == 3);  // mid-range fallback
    REQUIRE(f.p().projectedPercentCurrent(f.state) == 50);
}

TEST_CASE("FanMode read reflects the device speed range", "[fan_spec]")
{
    Fixture f(onCool());
    f.rec.applyOperationalObservation(obs(FanMode::Quiet));
    REQUIRE(f.p().projectedFanMode(f.state) == FanModeCategory::Low);    // 1 → Low
    f.rec.applyOperationalObservation(obs(FanMode::Medium));
    REQUIRE(f.p().projectedFanMode(f.state) == FanModeCategory::Medium); // 4 → Medium
    f.rec.applyOperationalObservation(obs(FanMode::High));
    REQUIRE(f.p().projectedFanMode(f.state) == FanModeCategory::High);   // 6 → High
}

TEST_CASE("FanMode/SpeedSetting/PercentSetting stay mutually consistent", "[fan_spec]")
{
    SECTION("SpeedSetting write derives FanMode + PercentSetting") {
        Fixture f(onCool());
        f.rec.applyIntent(SetSpeedSettingIntent{FanSpeed{FanLevel::Medium}}); // 4
        REQUIRE(f.p().projectedSpeedSetting(f.state)   == std::optional<uint8_t>{4});
        REQUIRE(f.p().projectedFanMode(f.state)        == FanModeCategory::Medium);
        REQUIRE(f.p().projectedPercentSetting(f.state) == std::optional<uint8_t>{66});
    }
    SECTION("PercentSetting write derives FanMode + SpeedSetting") {
        Fixture f(onCool());
        f.rec.applyIntent(SetPercentSettingIntent{50});
        REQUIRE(f.p().projectedSpeedSetting(f.state)   == std::optional<uint8_t>{3}); // ceil(6·0.5)
        REQUIRE(f.p().projectedFanMode(f.state)        == FanModeCategory::Medium);
        REQUIRE(f.p().projectedPercentSetting(f.state) == std::optional<uint8_t>{50}); // exact
    }
    SECTION("FanMode write (via representative speed) derives Speed + Percent") {
        Fixture f(onCool());
        f.rec.applyIntent(SetSpeedSettingIntent{FanSpeed{representativeSpeed(FanModeCategory::High)}});
        REQUIRE(f.p().projectedSpeedSetting(f.state)   == std::optional<uint8_t>{6});
        REQUIRE(f.p().projectedFanMode(f.state)        == FanModeCategory::High);
        REQUIRE(f.p().projectedPercentSetting(f.state) == std::optional<uint8_t>{100});
    }
}
