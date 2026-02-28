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

    mock.nextResponse = bytes(0x47, 0x31, 0x31, 0x33, 0x48, 0x41); // G1 response
    pres.getOperation([](auto) {});

    REQUIRE(mock.lastTransmitted == bytes(0x46, 0x31));
}

TEST_CASE("S21Presentation getOperation parses G1 on/Cool/22°C/Auto", "[s21pres][getOp]")
{
    // Preset response payload (G1): 47 31 31 33 48 41
    //   power='1'(on), mode='3'(Cool), temp=0x48('H'→22°C→2200), fan='A'(Auto)
    MockS21DataLink mock;
    S21Presentation pres(mock);

    mock.nextResponse = bytes(0x47, 0x31, 0x31, 0x33, 0x48, 0x41);

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

    mock.nextResponse = bytes(0x47, 0x31, 0x30, 0x34, 0x4E, 0x42);

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
