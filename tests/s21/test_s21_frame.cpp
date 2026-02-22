/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#include <catch2/catch_test_macros.hpp>

#include "S21Frame.h"

// Helper: build a std::vector<std::byte> from a braced list of integer literals.
template<typename... Args>
static std::vector<std::byte> bytes(Args... args)
{
    return {static_cast<std::byte>(args)...};
}

// ─── encode ──────────────────────────────────────────────────────────────────

TEST_CASE("S21Frame encode F1 poll frame", "[s21frame][encode]")
{
    // Payload: 'F'=0x46, '1'=0x31
    // Checksum: (0x46 + 0x31) & 0xFF = 0x77  — not a reserved byte
    // Expected: 02 46 31 77 03
    auto frame = S21Frame::encode(bytes(0x46, 0x31));
    REQUIRE(frame == bytes(0x02, 0x46, 0x31, 0x77, 0x03));
}

TEST_CASE("S21Frame encode D1 set on/Cool/22°C/Auto frame", "[s21frame][encode]")
{
    // Payload: 'D'=0x44, '1'=0x31, power=0x01, mode=0x03, temp=0x48('H'), fan=0x41('A')
    // Checksum: (0x44+0x31+0x01+0x03+0x48+0x41) & 0xFF = 0x102 & 0xFF = 0x02
    // 0x02 is STX (reserved) → add 2 → 0x04
    // Expected: 02 44 31 01 03 48 41 04 03
    auto frame = S21Frame::encode(bytes(0x44, 0x31, 0x01, 0x03, 0x48, 0x41));
    REQUIRE(frame == bytes(0x02, 0x44, 0x31, 0x01, 0x03, 0x48, 0x41, 0x04, 0x03));
}

TEST_CASE("S21Frame encode D1 set off/Cool/22°C/Auto frame", "[s21frame][encode]")
{
    // Payload: 'D'=0x44, '1'=0x31, power=0x00, mode=0x03, temp=0x48('H'), fan=0x41('A')
    // Checksum: (0x44+0x31+0x00+0x03+0x48+0x41) & 0xFF = 0x101 & 0xFF = 0x01
    // 0x01 is not reserved → checksum = 0x01
    // Expected: 02 44 31 00 03 48 41 01 03
    auto frame = S21Frame::encode(bytes(0x44, 0x31, 0x00, 0x03, 0x48, 0x41));
    REQUIRE(frame == bytes(0x02, 0x44, 0x31, 0x00, 0x03, 0x48, 0x41, 0x01, 0x03));
}

// ─── decode ──────────────────────────────────────────────────────────────────

TEST_CASE("S21Frame decode G1 on/Cool/22°C/Auto frame", "[s21frame][decode]")
{
    // Full frame: 02 47 31 01 03 48 41 05 03
    // Payload: 'G'=0x47, '1'=0x31, power=0x01, mode=0x03, temp=0x48, fan=0x41
    // Checksum: (0x47+0x31+0x01+0x03+0x48+0x41) & 0xFF = 0x105 & 0xFF = 0x05  ✓
    auto result = S21Frame::decode(bytes(0x02, 0x47, 0x31, 0x01, 0x03, 0x48, 0x41, 0x05, 0x03));
    REQUIRE(result.has_value());
    REQUIRE(*result == bytes(0x47, 0x31, 0x01, 0x03, 0x48, 0x41));
}

TEST_CASE("S21Frame decode returns error on bad checksum", "[s21frame][decode]")
{
    // Valid F1 frame with checksum corrupted from 0x77 → 0x00
    auto result = S21Frame::decode(bytes(0x02, 0x46, 0x31, 0x00, 0x03));
    REQUIRE(!result.has_value());
}

TEST_CASE("S21Frame decode returns error on missing STX", "[s21frame][decode]")
{
    // Frame missing the leading 0x02
    auto result = S21Frame::decode(bytes(0x46, 0x31, 0x77, 0x03));
    REQUIRE(!result.has_value());
}

TEST_CASE("S21Frame decode returns error on missing ETX", "[s21frame][decode]")
{
    // Frame missing the trailing 0x03
    auto result = S21Frame::decode(bytes(0x02, 0x46, 0x31, 0x77));
    REQUIRE(!result.has_value());
}
