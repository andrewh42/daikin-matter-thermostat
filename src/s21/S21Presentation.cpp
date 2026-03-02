/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#include "S21Presentation.h"

#include <cerrno>
#include <cstdint>

S21Presentation::~S21Presentation() = default;

/// @brief Converts a setPoint (0.01 °C units) to an S21 temperature byte.
/// S21 step = 0.5 °C = 50 units; base '@' (0x40) = 18 °C = 1800 units.
static uint8_t setPointToTempByte(int16_t setPoint)
{
    return static_cast<uint8_t>('@') + static_cast<uint8_t>((setPoint - 1800) / 50);
}

/// @brief Converts an S21 temperature byte to a setPoint (0.01 °C units).
static int16_t tempByteToSetPoint(uint8_t tempByte)
{
    return static_cast<int16_t>((tempByte - static_cast<uint8_t>('@')) * 50 + 1800);
}

void S21Presentation::setOperation(bool onOff, OperatingMode mode, int16_t setPoint, FanMode fanMode,
                                   SetOperationCallback cb)
{
    if (setPoint < 1000 || setPoint > 3200) {
        cb(tl::unexpected(S21DataLinkError("setPoint out of range")));
        return;
    }

    uint8_t power = onOff ? '1' : '0';
    uint8_t modeByte = static_cast<uint8_t>(mode);
    uint8_t tempByte = setPointToTempByte(setPoint);
    uint8_t fanByte = static_cast<uint8_t>(fanMode);

    m_dataLink.send(
            {
                    std::byte{'D'},
                    std::byte{'1'},
                    std::byte{power},
                    std::byte{modeByte},
                    std::byte{tempByte},
                    std::byte{fanByte},
            },
            std::move(cb));
}

/// @brief Reads 3 reversed ASCII decimal digits at r[offset..offset+2].
/// d0 is units (LSB), d1 is tens, d2 is hundreds.
static int decodeReversedDigits(const std::vector<std::byte>& r, size_t offset)
{
    return (static_cast<uint8_t>(r[offset])     - '0')
         + (static_cast<uint8_t>(r[offset + 1]) - '0') * 10
         + (static_cast<uint8_t>(r[offset + 2]) - '0') * 100;
}

/// @brief Decodes a 4-byte reversed ASCII sensor temperature starting at r[offset].
/// Format: d0 d1 d2 sign, where d0 is units (LSB). Value is in 0.1 °C; returns 0.01 °C.
static int16_t decodeSensorTemp(const std::vector<std::byte>& r, size_t offset = 2)
{
    int val = decodeReversedDigits(r, offset);
    bool positive = r[offset + 3] == std::byte{'+'};
    return static_cast<int16_t>((positive ? val : -val) * 10);
}

/// @brief Decodes a 3-byte reversed ASCII humidity value starting at r[offset].
/// Format: d0 d1 d2, where d0 is the units digit (LSB). Returns percentage.
static uint8_t decodeHumidity(const std::vector<std::byte>& r, size_t offset = 2)
{
    return static_cast<uint8_t>(decodeReversedDigits(r, offset));
}

/// @brief Decodes a single ASCII version digit byte; returns 0 for null padding.
static uint8_t decodeVersionDigit(std::byte b)
{
    uint8_t c = static_cast<uint8_t>(b);
    return (c >= '0' && c <= '9') ? static_cast<uint8_t>(c - '0') : 0;
}

void S21Presentation::getOperation(GetOperationCallback cb)
{
    m_dataLink.transact({std::byte{'F'}, std::byte{'1'}},
                        [cb = std::move(cb)](tl::expected<std::vector<std::byte>, S21DataLinkError> response) {
                            if (!response) {
                                cb(tl::unexpected(response.error()));
                                return;
                            }

                            bool onOff = (*response)[2] != std::byte{'0'};
                            auto mode = static_cast<OperatingMode>(static_cast<uint8_t>((*response)[3]));
                            int16_t setPoint = tempByteToSetPoint(static_cast<uint8_t>((*response)[4]));
                            FanMode fanMode = static_cast<FanMode>(static_cast<uint8_t>((*response)[5]));

                            cb(std::make_tuple(onOff, mode, setPoint, fanMode));
                        });
}

void S21Presentation::getRoomTemperature(GetTemperatureCallback cb)
{
    m_dataLink.transact({std::byte{'R'}, std::byte{'H'}},
                        [cb = std::move(cb)](tl::expected<std::vector<std::byte>, S21DataLinkError> response) {
                            if (!response) {
                                cb(tl::unexpected(response.error()));
                                return;
                            }
                            cb(decodeSensorTemp(*response));
                        });
}

void S21Presentation::getOutdoorTemperature(GetTemperatureCallback cb)
{
    m_dataLink.transact({std::byte{'R'}, std::byte{'a'}},
                        [cb = std::move(cb)](tl::expected<std::vector<std::byte>, S21DataLinkError> response) {
                            if (!response) {
                                cb(tl::unexpected(response.error()));
                                return;
                            }
                            cb(decodeSensorTemp(*response));
                        });
}

void S21Presentation::getHumidity(GetHumidityCallback cb)
{
    m_dataLink.transact({std::byte{'R'}, std::byte{'e'}},
                        [cb = std::move(cb)](tl::expected<std::vector<std::byte>, S21DataLinkError> response) {
                            if (!response) {
                                cb(tl::unexpected(response.error()));
                                return;
                            }
                            cb(decodeHumidity(*response));
                        });
}

void S21Presentation::getCoarseTemperatureAndHumidity(GetCoarseTemperatureAndHumidityCallback cb)
{
    m_dataLink.transact({std::byte{'F'}, std::byte{'9'}},
                        [cb = std::move(cb)](tl::expected<std::vector<std::byte>, S21DataLinkError> response) {
                            if (!response) {
                                cb(tl::unexpected(response.error()));
                                return;
                            }
                            int16_t indoor  = static_cast<int16_t>(static_cast<int8_t>(static_cast<uint8_t>((*response)[2]) - 0x80)) * 50;
                            int16_t outdoor = static_cast<int16_t>(static_cast<int8_t>(static_cast<uint8_t>((*response)[3]) - 0x80)) * 50;
                            uint8_t hum     = static_cast<uint8_t>((*response)[4]) - 0x30;
                            cb(std::make_tuple(indoor, outdoor, hum));
                        });
}

void S21Presentation::getFanMode(GetFanModeCallback cb)
{
    m_dataLink.transact({std::byte{'R'}, std::byte{'G'}},
                        [cb = std::move(cb)](tl::expected<std::vector<std::byte>, S21DataLinkError> response) {
                            if (!response) {
                                cb(tl::unexpected(response.error()));
                                return;
                            }
                            cb(static_cast<FanMode>(static_cast<uint8_t>((*response)[2])));
                        });
}

void S21Presentation::getProtocolVersion(GetProtocolVersionCallback cb)
{
    m_dataLink.transact({std::byte{'F'}, std::byte{'8'}},
                        [cb = std::move(cb)](tl::expected<std::vector<std::byte>, S21DataLinkError> response) {
                            if (!response) {
                                cb(tl::unexpected(response.error()));
                                return;
                            }
                            uint8_t minor = decodeVersionDigit((*response)[2]);
                            uint8_t major = decodeVersionDigit((*response)[3]);
                            cb(std::make_pair(major, minor));
                        });
}

void S21Presentation::getExtendedProtocolVersion(GetProtocolVersionCallback cb)
{
    m_dataLink.transact(
            {std::byte{'F'}, std::byte{'Y'}, std::byte{'0'}, std::byte{'0'}},
            [cb = std::move(cb)](tl::expected<std::vector<std::byte>, S21DataLinkError> response) {
                if (!response) {
                    cb(tl::unexpected(response.error()));
                    return;
                }
                uint8_t minor = decodeVersionDigit((*response)[4]) + decodeVersionDigit((*response)[5]) * 10;
                uint8_t major = decodeVersionDigit((*response)[6]) + decodeVersionDigit((*response)[7]) * 10;
                cb(std::make_pair(major, minor));
            });
}
