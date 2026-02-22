/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#include "S21Presentation.h"

#include <cerrno>
#include <cstdint>

S21Presentation::~S21Presentation() = default;

// setPoint (0.01°C units) → S21 temperature byte
// S21 step = 0.5°C = 50 units; base '@' (0x40) = 18°C = 1800 units
static uint8_t setPointToTempByte(int16_t setPoint)
{
    return static_cast<uint8_t>('@') + static_cast<uint8_t>((setPoint - 1800) / 50);
}

// S21 temperature byte → setPoint (0.01°C units)
static int16_t tempByteToSetPoint(uint8_t tempByte)
{
    return static_cast<int16_t>((tempByte - static_cast<uint8_t>('@')) * 50 + 1800);
}


int S21Presentation::setOperation(bool onOff, OperatingMode mode, int16_t setPoint, FanMode fanMode)
{
    if (setPoint < 1000 || setPoint > 3200) {
        return -EINVAL;
    }

    uint8_t power    = onOff ? '1' : '0';
    uint8_t modeByte = static_cast<uint8_t>(mode);
    uint8_t tempByte = setPointToTempByte(setPoint);
    uint8_t fanByte  = static_cast<uint8_t>(fanMode);

    m_dataLink.encodeAndTransmit({
        std::byte{'D'}, std::byte{'1'},
        std::byte{power}, std::byte{modeByte},
        std::byte{tempByte}, std::byte{fanByte},
    });

    return 0;
}

tl::expected<std::tuple<bool, OperatingMode, int16_t, FanMode>, S21DataLinkError>
S21Presentation::getOperation()
{
    m_dataLink.encodeAndTransmit({std::byte{'F'}, std::byte{'1'}});

    auto response = m_dataLink.receiveAndDecode();
    if (!response) {
        return tl::unexpected(response.error());
    }

    bool onOff       = (*response)[2] != std::byte{'0'};
    auto mode        = static_cast<OperatingMode>(static_cast<uint8_t>((*response)[3]));
    int16_t setPoint = tempByteToSetPoint(static_cast<uint8_t>((*response)[4]));
    FanMode fanMode  = static_cast<FanMode>(static_cast<uint8_t>((*response)[5]));

    return std::make_tuple(onOff, mode, setPoint, fanMode);
}
