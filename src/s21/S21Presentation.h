/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#pragma once

#include "S21DataLink.h"

#include <stdint.h>
#include <tuple>

enum class OperatingMode {
    Auto_Cooling = 0,
    Auto,
    Dry,
    Cool,
    Heat,
    FanOnly = 6,
    Auto_Heating,
};

enum class FanMode {
    Low,
    MidLow,
    Medium,
    MidHigh,
    High,
    Auto,
    Quiet,
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
