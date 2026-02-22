/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#pragma once

#include "S21DataLink.h"

#include <stdint.h>
#include <tuple>

enum class OperatingMode {
    Auto_Cooling = '0',  // 0x30
    Auto         = '1',  // 0x31
    Dry          = '2',  // 0x32
    Cool         = '3',  // 0x33
    Heat         = '4',  // 0x34
    FanOnly      = '6',  // 0x36
    Auto_Heating = '7',  // 0x37
};

enum class FanMode {
    Low     = '3',  // 0x33
    MidLow  = '4',  // 0x34
    Medium  = '5',  // 0x35
    MidHigh = '6',  // 0x36
    High    = '7',  // 0x37
    Auto    = 'A',  // 0x41
    Quiet   = 'B',  // 0x42
};

/**
 * @brief Presentation layer for Daikin S21 interface. Responsible for encoding commands to the S21 data link
 * and decoding responses.
 */
class S21Presentation {
public:
    S21Presentation(S21DataLink& dataLink) : m_dataLink(dataLink) {};
    ~S21Presentation();

    /// @brief Sets the operation mode.
    /// @param onOff true for ON, false for OFF.
    /// @param mode cooling/heating mode.
    /// @param setPoint temperature set point in hundredths of Celsius degrees (e.g. 2050 => 20.50°C).
    /// @param fanMode fan operating mode.
    /// @return 0 on success, negative error code on failure.
    int setOperation(bool onOff, OperatingMode mode, int16_t setPoint, FanMode fanMode);

    tl::expected<std::tuple<bool, OperatingMode, int16_t, FanMode>, S21DataLinkError> getOperation();

private:
    S21DataLink& m_dataLink;
};
