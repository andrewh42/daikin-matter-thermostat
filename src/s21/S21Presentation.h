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
    void setOperation(bool onOff, OperatingMode mode, int16_t setPoint, FanMode fanMode);
    std::tuple<bool, OperatingMode, int16_t, FanMode> getOperation();

private:
    S21DataLink& m_dataLink;
};
