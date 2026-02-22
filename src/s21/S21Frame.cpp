/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#include "S21Frame.h"

#include <cstdint>

namespace S21Frame {

static constexpr uint8_t kReserved[] = {0x02, 0x03, 0x06, 0x0A, 0x15};

static uint8_t computeChecksum(const std::vector<std::byte>& payload)
{
    uint8_t sum = 0;
    for (auto b : payload) {
        sum += static_cast<uint8_t>(b);
    }
    for (auto r : kReserved) {
        if (sum == r) {
            sum += 2;
            break;
        }
    }
    return sum;
}

std::vector<std::byte> encode(const std::vector<std::byte>& payload)
{
    std::vector<std::byte> frame;
    frame.reserve(payload.size() + 3);
    frame.push_back(std::byte{0x02});
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(std::byte{computeChecksum(payload)});
    frame.push_back(std::byte{0x03});
    return frame;
}

tl::expected<std::vector<std::byte>, FrameError>
    decode(const std::vector<std::byte>& frame)
{
    if (frame.size() < 4) {
        return tl::unexpected(FrameError("frame too short"));
    }
    if (frame.front() != std::byte{0x02}) {
        return tl::unexpected(FrameError("missing STX"));
    }
    if (frame.back() != std::byte{0x03}) {
        return tl::unexpected(FrameError("missing ETX"));
    }

    // Layout: [STX] [payload...] [checksum] [ETX]
    std::vector<std::byte> payload(frame.begin() + 1, frame.end() - 2);
    auto checksumByte = static_cast<uint8_t>(frame[frame.size() - 2]);

    if (computeChecksum(payload) != checksumByte) {
        return tl::unexpected(FrameError("bad checksum"));
    }

    return payload;
}

} // namespace S21Frame
