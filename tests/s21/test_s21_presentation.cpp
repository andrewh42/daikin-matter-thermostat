/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include "s21_presentation.h"
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

    tl::expected<S21Presentation::GetOperationResult, S21PresentationError> result;
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

    tl::expected<S21Presentation::GetOperationResult, S21PresentationError> result;
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

    tl::expected<S21Presentation::GetOperationResult, S21PresentationError> result;
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

    tl::expected<S21Presentation::GetTemperatureResult, S21PresentationError> result;
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

    tl::expected<S21Presentation::GetTemperatureResult, S21PresentationError> result;
    pres.getRoomTemperature([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(result.value() == -500);
}

TEST_CASE("S21Presentation getRoomTemperature parses zero", "[s21pres][roomTemp]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('S', 'H', '0', '0', '0', '+');

    tl::expected<S21Presentation::GetTemperatureResult, S21PresentationError> result;
    pres.getRoomTemperature([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(result.value() == 0);
}

TEST_CASE("S21Presentation getRoomTemperature propagates transact error", "[s21pres][roomTemp]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextError = S21DataLinkError("timeout");

    tl::expected<S21Presentation::GetTemperatureResult, S21PresentationError> result;
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

    tl::expected<S21Presentation::GetTemperatureResult, S21PresentationError> result;
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

    tl::expected<S21Presentation::GetTemperatureResult, S21PresentationError> result;
    pres.getOutdoorTemperature([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(result.value() == -1050);
}

TEST_CASE("S21Presentation getOutdoorTemperature parses zero", "[s21pres][outdoorTemp]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('S', 'a', '0', '0', '0', '+');

    tl::expected<S21Presentation::GetTemperatureResult, S21PresentationError> result;
    pres.getOutdoorTemperature([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(result.value() == 0);
}

TEST_CASE("S21Presentation getOutdoorTemperature propagates transact error", "[s21pres][outdoorTemp]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextError = S21DataLinkError("timeout");

    tl::expected<S21Presentation::GetTemperatureResult, S21PresentationError> result;
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

    tl::expected<S21Presentation::GetHumidityResult, S21PresentationError> result;
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

    tl::expected<S21Presentation::GetHumidityResult, S21PresentationError> result;
    pres.getHumidity([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(result.value() == 50);
}

TEST_CASE("S21Presentation getHumidity parses 0%", "[s21pres][humidity]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('S', 'e', '0', '0', '0');

    tl::expected<S21Presentation::GetHumidityResult, S21PresentationError> result;
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

    tl::expected<S21Presentation::GetHumidityResult, S21PresentationError> result;
    pres.getHumidity([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(result.value() == 100);
}

TEST_CASE("S21Presentation getHumidity propagates transact error", "[s21pres][humidity]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextError = S21DataLinkError("timeout");

    tl::expected<S21Presentation::GetHumidityResult, S21PresentationError> result;
    pres.getHumidity([&](auto r) { result = r; });

    REQUIRE_FALSE(result.has_value());
}

// ─── getCoarseTemperatureAndHumidity ─────────────────────────────────────────

TEST_CASE("S21Presentation getCoarseTemperatureAndHumidity sends F9 poll", "[s21pres][coarseTemp]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('G', '9', 0xAC, 0xAA, 0x4E, 0x30);
    pres.getCoarseTemperatureAndHumidity([](auto) {});

    REQUIRE(mock.lastTransmitted == bytes('F', '9'));
}

TEST_CASE("S21Presentation getCoarseTemperatureAndHumidity parses 22°C indoor, 21°C outdoor, 30% humidity", "[s21pres][coarseTemp]")
{
    // G9 response: 0xAC=indoor(22°C→2200), 0xAA=outdoor(21°C→2100), 0x4E=humidity(0x4E-0x30=30%)
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('G', '9', 0xAC, 0xAA, 0x4E, 0x30);

    tl::expected<S21Presentation::GetCoarseTemperatureAndHumidityResult, S21PresentationError> result;
    pres.getCoarseTemperatureAndHumidity([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    auto [indoor, outdoor, hum] = result.value();
    REQUIRE(indoor == 2200);
    REQUIRE(outdoor == 2100);
    REQUIRE(hum == 30);
}

TEST_CASE("S21Presentation getCoarseTemperatureAndHumidity parses negative outdoor temp -5°C", "[s21pres][coarseTemp]")
{
    // outdoor byte 0x76: (0x76 - 0x80) as int8_t = -10; -10 * 50 = -500 (= -5.00°C)
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('G', '9', 0xAC, 0x76, 0x4E, 0x30);

    tl::expected<S21Presentation::GetCoarseTemperatureAndHumidityResult, S21PresentationError> result;
    pres.getCoarseTemperatureAndHumidity([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    auto [indoor, outdoor, hum] = result.value();
    REQUIRE(outdoor == -500);
}

TEST_CASE("S21Presentation getCoarseTemperatureAndHumidity propagates transact error", "[s21pres][coarseTemp]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextError = S21DataLinkError("timeout");

    tl::expected<S21Presentation::GetCoarseTemperatureAndHumidityResult, S21PresentationError> result;
    pres.getCoarseTemperatureAndHumidity([&](auto r) { result = r; });

    REQUIRE_FALSE(result.has_value());
}

// ─── getFanMode ───────────────────────────────────────────────────────────────

TEST_CASE("S21Presentation getFanMode sends RG poll", "[s21pres][fanMode]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('S', 'G', '7');
    pres.getFanMode([](auto) {});

    REQUIRE(mock.lastTransmitted == bytes('R', 'G'));
}

TEST_CASE("S21Presentation getFanMode parses High", "[s21pres][fanMode]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('S', 'G', '7');

    tl::expected<S21Presentation::GetFanModeResult, S21PresentationError> result;
    pres.getFanMode([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(result.value() == FanMode::High);
}

TEST_CASE("S21Presentation getFanMode parses Quiet", "[s21pres][fanMode]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('S', 'G', 'B');

    tl::expected<S21Presentation::GetFanModeResult, S21PresentationError> result;
    pres.getFanMode([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(result.value() == FanMode::Quiet);
}

TEST_CASE("S21Presentation getFanMode propagates transact error", "[s21pres][fanMode]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextError = S21DataLinkError("timeout");

    tl::expected<S21Presentation::GetFanModeResult, S21PresentationError> result;
    pres.getFanMode([&](auto r) { result = r; });

    REQUIRE_FALSE(result.has_value());
}

// ─── getProtocolVersion ───────────────────────────────────────────────────────

TEST_CASE("S21Presentation getProtocolVersion sends F8 poll", "[s21pres][protVer]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('G', '8', '0', 0x00, 0x00, 0x00);
    pres.getProtocolVersion([](auto) {});

    REQUIRE(mock.lastTransmitted == bytes('F', '8'));
}

TEST_CASE("S21Presentation getProtocolVersion parses v0.0 single digit", "[s21pres][protVer]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('G', '8', '0', 0x00, 0x00, 0x00);

    tl::expected<S21Presentation::GetProtocolVersionResult, S21PresentationError> result;
    pres.getProtocolVersion([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(result.value().first == 0);  // major
    REQUIRE(result.value().second == 0); // minor
}

TEST_CASE("S21Presentation getProtocolVersion parses v1.0 two digits", "[s21pres][protVer]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('G', '8', '0', '1', 0x00, 0x00);

    tl::expected<S21Presentation::GetProtocolVersionResult, S21PresentationError> result;
    pres.getProtocolVersion([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(result.value().first == 1);  // major
    REQUIRE(result.value().second == 0); // minor
}

TEST_CASE("S21Presentation getProtocolVersion parses v2.0 two digits", "[s21pres][protVer]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('G', '8', '0', '2', 0x00, 0x00);

    tl::expected<S21Presentation::GetProtocolVersionResult, S21PresentationError> result;
    pres.getProtocolVersion([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(result.value().first == 2);  // major
    REQUIRE(result.value().second == 0); // minor
}

TEST_CASE("S21Presentation getProtocolVersion propagates transact error", "[s21pres][protVer]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextError = S21DataLinkError("timeout");

    tl::expected<S21Presentation::GetProtocolVersionResult, S21PresentationError> result;
    pres.getProtocolVersion([&](auto r) { result = r; });

    REQUIRE_FALSE(result.has_value());
}

// ─── getExtendedProtocolVersion ───────────────────────────────────────────────

TEST_CASE("S21Presentation getExtendedProtocolVersion sends FY00 poll", "[s21pres][extProtVer]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('G', 'Y', '0', '0', '0', '2', '3', '0');
    pres.getExtendedProtocolVersion([](auto) {});

    REQUIRE(mock.lastTransmitted == bytes('F', 'Y', '0', '0'));
}

TEST_CASE("S21Presentation getExtendedProtocolVersion parses v3.20", "[s21pres][extProtVer]")
{
    // GY00 response data '0','2','3','0': minor=0+2*10=20, major=3+0*10=3 → v3.20
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('G', 'Y', '0', '0', '0', '2', '3', '0');

    tl::expected<S21Presentation::GetProtocolVersionResult, S21PresentationError> result;
    pres.getExtendedProtocolVersion([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(result.value().first == 3);   // major
    REQUIRE(result.value().second == 20); // minor
}

TEST_CASE("S21Presentation getExtendedProtocolVersion NAK on v2 and below units", "[s21pres][extProtVer]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextError = S21DataLinkError("NAK");

    tl::expected<S21Presentation::GetProtocolVersionResult, S21PresentationError> result;
    pres.getExtendedProtocolVersion([&](auto r) { result = r; });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(std::string_view(result.error().what()) == "NAK");
}

TEST_CASE("S21Presentation getExtendedProtocolVersion propagates transact error", "[s21pres][extProtVer]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextError = S21DataLinkError("timeout");

    tl::expected<S21Presentation::GetProtocolVersionResult, S21PresentationError> result;
    pres.getExtendedProtocolVersion([&](auto r) { result = r; });

    REQUIRE_FALSE(result.has_value());
}

// ─── wrong response code tests ────────────────────────────────────────────────

TEST_CASE("S21Presentation getOperation rejects wrong response code", "[s21pres][getOp]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('X', 'X', '1', '3', 'H', 'A');

    tl::expected<S21Presentation::GetOperationResult, S21PresentationError> result;
    pres.getOperation([&](auto r) { result = r; });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(std::string_view(result.error().what()) == "unexpected response code");
}

TEST_CASE("S21Presentation getRoomTemperature rejects wrong response code", "[s21pres][roomTemp]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('X', 'X', '5', '6', '2', '+');

    tl::expected<S21Presentation::GetTemperatureResult, S21PresentationError> result;
    pres.getRoomTemperature([&](auto r) { result = r; });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(std::string_view(result.error().what()) == "unexpected response code");
}

TEST_CASE("S21Presentation getOutdoorTemperature rejects wrong response code", "[s21pres][outdoorTemp]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('X', 'X', '5', '2', '0', '+');

    tl::expected<S21Presentation::GetTemperatureResult, S21PresentationError> result;
    pres.getOutdoorTemperature([&](auto r) { result = r; });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(std::string_view(result.error().what()) == "unexpected response code");
}

TEST_CASE("S21Presentation getHumidity rejects wrong response code", "[s21pres][humidity]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('X', 'X', '9', '7', '0');

    tl::expected<S21Presentation::GetHumidityResult, S21PresentationError> result;
    pres.getHumidity([&](auto r) { result = r; });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(std::string_view(result.error().what()) == "unexpected response code");
}

TEST_CASE("S21Presentation getCoarseTemperatureAndHumidity rejects wrong response code", "[s21pres][coarseTemp]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('X', 'X', 0xAC, 0xAA, 0x4E, 0x30);

    tl::expected<S21Presentation::GetCoarseTemperatureAndHumidityResult, S21PresentationError> result;
    pres.getCoarseTemperatureAndHumidity([&](auto r) { result = r; });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(std::string_view(result.error().what()) == "unexpected response code");
}

TEST_CASE("S21Presentation getFanMode rejects wrong response code", "[s21pres][fanMode]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('X', 'X', '7');

    tl::expected<S21Presentation::GetFanModeResult, S21PresentationError> result;
    pres.getFanMode([&](auto r) { result = r; });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(std::string_view(result.error().what()) == "unexpected response code");
}

TEST_CASE("S21Presentation getProtocolVersion rejects wrong response code", "[s21pres][protVer]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('X', 'X', '0', '2', 0x00, 0x00);

    tl::expected<S21Presentation::GetProtocolVersionResult, S21PresentationError> result;
    pres.getProtocolVersion([&](auto r) { result = r; });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(std::string_view(result.error().what()) == "unexpected response code");
}

TEST_CASE("S21Presentation getExtendedProtocolVersion rejects wrong response code", "[s21pres][extProtVer]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('X', 'X', '0', '0', '0', '2', '3', '0');

    tl::expected<S21Presentation::GetProtocolVersionResult, S21PresentationError> result;
    pres.getExtendedProtocolVersion([&](auto r) { result = r; });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(std::string_view(result.error().what()) == "unexpected response code");
}

// ─── getUnitState ─────────────────────────────────────────────────────────────

TEST_CASE("S21Presentation getUnitState sends RzB2 poll", "[s21pres][unitState]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('S', 'z', 'B', '2', '0', '0');
    pres.getUnitState([](auto) {});

    REQUIRE(mock.lastTransmitted == bytes('R', 'z', 'B', '2'));
}

TEST_CASE("S21Presentation getUnitState parses offline SzB200 (all bits clear)", "[s21pres][unitState]")
{
    // SzB200: payload '0','0' → bits = 0x00 → all false
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('S', 'z', 'B', '2', '0', '0');

    tl::expected<S21Presentation::GetUnitStateResult, S21PresentationError> result;
    pres.getUnitState([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->powerful);
    REQUIRE_FALSE(result->defrost);
    REQUIRE_FALSE(result->refrigerantValveOpen);
    REQUIRE_FALSE(result->online);
}

TEST_CASE("S21Presentation getUnitState parses online SzB280 (bit 0x08)", "[s21pres][unitState]")
{
    // SzB280: payload '8','0' → hexNibble('8')|(hexNibble('0')<<4) = 0x08 → online only
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('S', 'z', 'B', '2', '8', '0');

    tl::expected<S21Presentation::GetUnitStateResult, S21PresentationError> result;
    pres.getUnitState([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->powerful);
    REQUIRE_FALSE(result->defrost);
    REQUIRE_FALSE(result->refrigerantValveOpen);
    REQUIRE(result->online);
}

TEST_CASE("S21Presentation getUnitState parses online+active SzB2C0 (bits 0x04+0x08)", "[s21pres][unitState]")
{
    // SzB2C0: payload 'C','0' → 0x0C → refrigerantValveOpen + online
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('S', 'z', 'B', '2', 'C', '0');

    tl::expected<S21Presentation::GetUnitStateResult, S21PresentationError> result;
    pres.getUnitState([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->powerful);
    REQUIRE_FALSE(result->defrost);
    REQUIRE(result->refrigerantValveOpen);
    REQUIRE(result->online);
}

TEST_CASE("S21Presentation getUnitState parses online+active+powerful SzB2D0 (bits 0x01+0x04+0x08)", "[s21pres][unitState]")
{
    // SzB2D0: payload 'D','0' → 0x0D → powerful + refrigerantValveOpen + online
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('S', 'z', 'B', '2', 'D', '0');

    tl::expected<S21Presentation::GetUnitStateResult, S21PresentationError> result;
    pres.getUnitState([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE(result->powerful);
    REQUIRE_FALSE(result->defrost);
    REQUIRE(result->refrigerantValveOpen);
    REQUIRE(result->online);
}

TEST_CASE("S21Presentation getUnitState parses defrost+online SzB2A0 (bits 0x02+0x08)", "[s21pres][unitState]")
{
    // SzB2A0: payload 'A','0' → 0x0A → defrost + online
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('S', 'z', 'B', '2', 'A', '0');

    tl::expected<S21Presentation::GetUnitStateResult, S21PresentationError> result;
    pres.getUnitState([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->powerful);
    REQUIRE(result->defrost);
    REQUIRE_FALSE(result->refrigerantValveOpen);
    REQUIRE(result->online);
}

TEST_CASE("S21Presentation getUnitState parses defrost+online+active SzB2E0 (bits 0x02+0x04+0x08)", "[s21pres][unitState]")
{
    // SzB2E0: payload 'E','0' → 0x0E → defrost + refrigerantValveOpen + online
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('S', 'z', 'B', '2', 'E', '0');

    tl::expected<S21Presentation::GetUnitStateResult, S21PresentationError> result;
    pres.getUnitState([&](auto r) { result = r; });

    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->powerful);
    REQUIRE(result->defrost);
    REQUIRE(result->refrigerantValveOpen);
    REQUIRE(result->online);
}

TEST_CASE("S21Presentation getUnitState rejects wrong 2-char response code", "[s21pres][unitState]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('X', 'X', 'B', '2', '0', '0');

    tl::expected<S21Presentation::GetUnitStateResult, S21PresentationError> result;
    pres.getUnitState([&](auto r) { result = r; });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(std::string_view(result.error().what()) == "unexpected response code");
}

TEST_CASE("S21Presentation getUnitState rejects wrong subcode bytes", "[s21pres][unitState]")
{
    // 'S','z' match but 'X','X' does not match 'B','2'
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextResponse = bytes('S', 'z', 'X', 'X', '0', '0');

    tl::expected<S21Presentation::GetUnitStateResult, S21PresentationError> result;
    pres.getUnitState([&](auto r) { result = r; });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(std::string_view(result.error().what()) == "unexpected response code");
}

TEST_CASE("S21Presentation getUnitState propagates transact error", "[s21pres][unitState]")
{
    MockS21DataLink mock;
    S21Presentation pres(mock);
    mock.nextError = S21DataLinkError("timeout");

    tl::expected<S21Presentation::GetUnitStateResult, S21PresentationError> result;
    pres.getUnitState([&](auto r) { result = r; });

    REQUIRE_FALSE(result.has_value());
    REQUIRE(std::string_view(result.error().what()) == "timeout");
}
