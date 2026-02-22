/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#pragma once

#include <tl/expected.hpp>

#include <cstddef>
#include <stdexcept>
#include <vector>

namespace S21Frame {

struct FrameError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/**
 * @brief Wrap payload bytes in STX / checksum / ETX.
 *
 * Frame format: [STX=0x02] [payload] [checksum] [ETX=0x03]
 * Checksum: sum of payload bytes & 0xFF; if result is a reserved byte
 * (0x02, 0x03, 0x06, 0x0A, 0x15), add 2.
 */
std::vector<std::byte> encode(const std::vector<std::byte>& payload);

/**
 * @brief Strip STX / checksum / ETX.
 *
 * Returns the payload on success, or a FrameError describing the problem.
 */
[[nodiscard]]
tl::expected<std::vector<std::byte>, FrameError>
    decode(const std::vector<std::byte>& frame);

} // namespace S21Frame
