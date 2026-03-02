/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include "S21Presentation.h"
#include "mock_s21_datalink.h"

// Helper: build a std::vector<std::byte> from a braced list of integer literals.
template<typename... Args>
static std::vector<std::byte> bytes(Args... args)
{
    return {static_cast<std::byte>(args)...};
}

// ─── setOperation ────────────────────────────────────────────────────────────

TEST_CASE("S21Presentation setOperation on/Cool/22°C/Auto sends D1 payload", "[s21pres][setOp]")
{
    // Power=On('1'=0x31), Mode=Cool('3'=0x33),
    // Temp: 22.00°C → '@'(0x40) + (22-18)*2 = 0x48 ('H')
    // Fan: Auto → 'A'(0x41)
    // Expected payload to send: 44 31 31 33 48 41
    MockS21DataLink mock;
    S21Presentation pres(mock);

    tl::expected<void, S21DataLinkError> result;
    pres.setOperation(true, OperatingMode::Cool, 2200, FanMode::Auto,
                      [&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(mock.lastTransmitted == bytes(0x44, 0x31, 0x31, 0x33, 0x48, 0x41));
}

TEST_CASE("S21Presentation setOperation off/Heat/25°C/Low sends D1 payload", "[s21pres][setOp]")
{
    // Power=Off('0'=0x30), Mode=Heat('4'=0x34),
    // Temp: 25.00°C → '@'(0x40) + (25-18)*2 = 0x4E ('N')
    // Fan: Low → '3'(0x33)
    // Expected payload to send: 44 31 30 34 4E 33
    MockS21DataLink mock;
    S21Presentation pres(mock);

    tl::expected<void, S21DataLinkError> result;
    pres.setOperation(false, OperatingMode::Heat, 2500, FanMode::Low,
                      [&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(mock.lastTransmitted == bytes(0x44, 0x31, 0x30, 0x34, 0x4E, 0x33));
}

TEST_CASE("S21Presentation setOperation on/Cool/16°C/Auto sends D1 payload", "[s21pres][setOp]")
{
    // 16.00°C = 1600 units
    // Temp: '@'(0x40) + (1600-1800)/50 = 0x40 + uint8_t(-4) = 0x40 + 0xFC = 0x3C ('<')
    // Expected payload: 44 31 31 33 3C 41
    MockS21DataLink mock;
    S21Presentation pres(mock);

    tl::expected<void, S21DataLinkError> result;
    pres.setOperation(true, OperatingMode::Cool, 1600, FanMode::Auto,
                      [&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(mock.lastTransmitted == bytes(0x44, 0x31, 0x31, 0x33, 0x3C, 0x41));
}

TEST_CASE("S21Presentation setOperation rejects setPoint below 10°C", "[s21pres][setOp]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);

    tl::expected<void, S21DataLinkError> result;
    pres.setOperation(true, OperatingMode::Cool, 500, FanMode::Auto,
                      [&](auto r) { result = r; });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(mock.lastTransmitted.empty());
}

TEST_CASE("S21Presentation setOperation rejects setPoint above 32°C", "[s21pres][setOp]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);

    tl::expected<void, S21DataLinkError> result;
    pres.setOperation(true, OperatingMode::Cool, 3500, FanMode::Auto,
                      [&](auto r) { result = r; });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(mock.lastTransmitted.empty());
}

// ─── getOperation ────────────────────────────────────────────────────────────

TEST_CASE("S21Presentation getOperation sends F1 poll payload", "[s21pres][getOp]")
{
    // getOperation must transmit the F1 poll: 'F'=0x46, '1'=0x31
    MockS21DataLink mock;
    S21Presentation pres(mock);

    mock.nextResponse = bytes('G', '1', '1', '3', 'H', 'A'); // G1 response
    pres.getOperation([](auto) {});

    REQUIRE(mock.lastTransmitted == bytes(0x46, 0x31));
}

TEST_CASE("S21Presentation getOperation parses G1 on/Cool/22°C/Auto", "[s21pres][getOp]")
{
    // Preset response payload (G1): 47 31 31 33 48 41
    //   power='1'(on), mode='3'(Cool), temp=0x48('H'→22°C→2200), fan='A'(Auto)
    MockS21DataLink mock;
    S21Presentation pres(mock);

    mock.nextResponse = bytes('G', '1', '1', '3', 'H', 'A');

    tl::expected<S21Presentation::GetOperationResult, S21DataLinkError> result;
    pres.getOperation([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    auto [onOff, mode, setPoint, fanMode] = result.value();

    REQUIRE(onOff == true);
    REQUIRE(mode == OperatingMode::Cool);
    REQUIRE(setPoint == 2200);
    REQUIRE(fanMode == FanMode::Auto);
}

TEST_CASE("S21Presentation getOperation parses G1 off/Heat/25°C/Quiet", "[s21pres][getOp]")
{
    // Preset response payload (G1): 47 31 30 34 4E 42
    //   power='0'(off), mode='4'(Heat), temp=0x4E('N'→25°C→2500), fan='B'(Quiet)
    MockS21DataLink mock;
    S21Presentation pres(mock);

    mock.nextResponse = bytes('G', '1', '0', '4', 'N', 'B');

    tl::expected<S21Presentation::GetOperationResult, S21DataLinkError> result;
    pres.getOperation([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    auto [onOff, mode, setPoint, fanMode] = result.value();

    REQUIRE(onOff == false);
    REQUIRE(mode == OperatingMode::Heat);
    REQUIRE(setPoint == 2500);
    REQUIRE(fanMode == FanMode::Quiet);
}

TEST_CASE("S21Presentation getOperation propagates transact error", "[s21pres][getOp]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextError = S21DataLinkError("timeout");

    tl::expected<S21Presentation::GetOperationResult, S21DataLinkError> result;
    pres.getOperation([&](auto r) { result = r; });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(std::string_view(result.error().what()) == "timeout");
}

// ─── getRoomTemperature ───────────────────────────────────────────────────────

TEST_CASE("S21Presentation getRoomTemperature sends RH poll", "[s21pres][roomTemp]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);

    mock.nextResponse = bytes('S', 'H', '5', '6', '2', '+'); // SH response
    pres.getRoomTemperature([](auto) {});

    REQUIRE(mock.lastTransmitted == bytes(0x52, 0x48)); // 'R', 'H'
}

TEST_CASE("S21Presentation getRoomTemperature parses positive temp 26.5°C", "[s21pres][roomTemp]")
{
    // SH response: '5','6','2','+' → d0=5, d1=6, d2=2 → 5+60+200=265 (0.1°C) × 10 = 2650
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('S', 'H', '5', '6', '2', '+');

    tl::expected<S21Presentation::GetTemperatureResult, S21DataLinkError> result;
    pres.getRoomTemperature([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(result.value() == 2650);
}

TEST_CASE("S21Presentation getRoomTemperature parses negative temp -5.0°C", "[s21pres][roomTemp]")
{
    // SH response: '0','5','0','-' → d0=0, d1=5, d2=0 → 50 (0.1°C) × 10 = 500, negative → -500
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('S', 'H', '0', '5', '0', '-');

    tl::expected<S21Presentation::GetTemperatureResult, S21DataLinkError> result;
    pres.getRoomTemperature([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(result.value() == -500);
}

TEST_CASE("S21Presentation getRoomTemperature parses zero", "[s21pres][roomTemp]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('S', 'H', '0', '0', '0', '+');

    tl::expected<S21Presentation::GetTemperatureResult, S21DataLinkError> result;
    pres.getRoomTemperature([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(result.value() == 0);
}

TEST_CASE("S21Presentation getRoomTemperature propagates transact error", "[s21pres][roomTemp]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextError = S21DataLinkError("timeout");

    tl::expected<S21Presentation::GetTemperatureResult, S21DataLinkError> result;
    pres.getRoomTemperature([&](auto r) { result = r; });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(std::string_view(result.error().what()) == "timeout");
}

// ─── getOutdoorTemperature ────────────────────────────────────────────────────

TEST_CASE("S21Presentation getOutdoorTemperature sends Ra poll", "[s21pres][outdoorTemp]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);

    mock.nextResponse = bytes('S', 'a', '5', '2', '0', '+'); // Sa response
    pres.getOutdoorTemperature([](auto) {});

    REQUIRE(mock.lastTransmitted == bytes(0x52, 0x61)); // 'R', 'a'
}

TEST_CASE("S21Presentation getOutdoorTemperature parses 2.5°C", "[s21pres][outdoorTemp]")
{
    // Sa response: '5','2','0','+' → d0=5, d1=2, d2=0 → 5+20+0=25 (0.1°C) × 10 = 250
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('S', 'a', '5', '2', '0', '+');

    tl::expected<S21Presentation::GetTemperatureResult, S21DataLinkError> result;
    pres.getOutdoorTemperature([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(result.value() == 250);
}

TEST_CASE("S21Presentation getOutdoorTemperature parses negative temp -10.5°C", "[s21pres][outdoorTemp]")
{
    // Sa response: '5','0','1','-' → d0=5, d1=0, d2=1 → 5+0+100=105 (0.1°C) × 10 = 1050, negative → -1050
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('S', 'a', '5', '0', '1', '-');

    tl::expected<S21Presentation::GetTemperatureResult, S21DataLinkError> result;
    pres.getOutdoorTemperature([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(result.value() == -1050);
}

TEST_CASE("S21Presentation getOutdoorTemperature parses zero", "[s21pres][outdoorTemp]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('S', 'a', '0', '0', '0', '+');

    tl::expected<S21Presentation::GetTemperatureResult, S21DataLinkError> result;
    pres.getOutdoorTemperature([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(result.value() == 0);
}

TEST_CASE("S21Presentation getOutdoorTemperature propagates transact error", "[s21pres][outdoorTemp]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextError = S21DataLinkError("timeout");

    tl::expected<S21Presentation::GetTemperatureResult, S21DataLinkError> result;
    pres.getOutdoorTemperature([&](auto r) { result = r; });

    REQUIRE_FALSE(result.has_value());
}

// ─── getHumidity ─────────────────────────────────────────────────────────────

TEST_CASE("S21Presentation getHumidity sends Re poll", "[s21pres][humidity]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);

    mock.nextResponse = bytes('S', 'e', '9', '7', '0'); // Se response
    pres.getHumidity([](auto) {});

    REQUIRE(mock.lastTransmitted == bytes(0x52, 0x65)); // 'R', 'e'
}

TEST_CASE("S21Presentation getHumidity parses 79%", "[s21pres][humidity]")
{
    // Se response: '9','7','0' → d0=9, d1=7, d2=0 → 9+70+0 = 79
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('S', 'e', '9', '7', '0');

    tl::expected<S21Presentation::GetHumidityResult, S21DataLinkError> result;
    pres.getHumidity([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(result.value() == 79);
}

TEST_CASE("S21Presentation getHumidity parses 50%", "[s21pres][humidity]")
{
    // Se response: '0','5','0' → d0=0, d1=5, d2=0 → 0+50+0 = 50
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('S', 'e', '0', '5', '0');

    tl::expected<S21Presentation::GetHumidityResult, S21DataLinkError> result;
    pres.getHumidity([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(result.value() == 50);
}

TEST_CASE("S21Presentation getHumidity parses 0%", "[s21pres][humidity]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('S', 'e', '0', '0', '0');

    tl::expected<S21Presentation::GetHumidityResult, S21DataLinkError> result;
    pres.getHumidity([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(result.value() == 0);
}

TEST_CASE("S21Presentation getHumidity parses 100%", "[s21pres][humidity]")
{
    // Se response: '0','0','1' → d0=0, d1=0, d2=1 → 0+0+100 = 100
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('S', 'e', '0', '0', '1');

    tl::expected<S21Presentation::GetHumidityResult, S21DataLinkError> result;
    pres.getHumidity([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(result.value() == 100);
}

TEST_CASE("S21Presentation getHumidity propagates transact error", "[s21pres][humidity]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextError = S21DataLinkError("timeout");

    tl::expected<S21Presentation::GetHumidityResult, S21DataLinkError> result;
    pres.getHumidity([&](auto r) { result = r; });

    REQUIRE_FALSE(result.has_value());
}
