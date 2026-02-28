/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#pragma once

#include "S21DataLink.h"

#include <functional>
#include <stdint.h>
#include <tuple>

enum class OperatingMode : uint8_t {
    Auto_Cooling = '0', // 0x30
    Auto = '1',         // 0x31
    Dry = '2',          // 0x32
    Cool = '3',         // 0x33
    Heat = '4',         // 0x34
    FanOnly = '6',      // 0x36
    Auto_Heating = '7', // 0x37
};

enum class FanMode : uint8_t {
    Low = '3',     // 0x33
    MidLow = '4',  // 0x34
    Medium = '5',  // 0x35
    MidHigh = '6', // 0x36
    High = '7',    // 0x37
    Auto = 'A',    // 0x41
    Quiet = 'B',   // 0x42
};

/**
 * @brief Presentation layer for Daikin S21 interface. Responsible for encoding commands to the S21
 * data link and decoding responses.
 */
class S21Presentation {
  public:
    using GetOperationResult = std::tuple<bool, OperatingMode, int16_t, FanMode>;
    using SetOperationCallback = std::function<void(tl::expected<void, S21DataLinkError>)>;
    using GetOperationCallback = std::function<void(tl::expected<GetOperationResult, S21DataLinkError>)>;

    S21Presentation(S21DataLink& dataLink)
            : m_dataLink(dataLink) {};
    ~S21Presentation();

    /// @brief Sets the operation mode.
    /// @param onOff true for ON, false for OFF.
    /// @param mode cooling/heating mode.
    /// @param setPoint temperature set point in hundredths of Celsius degrees (e.g. 2050
    /// => 20.50°C).
    /// @param fanMode fan operating mode.
    /// @param cb callback invoked with success or error when the AC acknowledges the command.
    void setOperation(bool onOff, OperatingMode mode, int16_t setPoint, FanMode fanMode, SetOperationCallback cb);

    /// @brief Gets the current operation mode.
    /// @param cb callback invoked with (onOff, mode, setPoint, fanMode) on success, or an error.
    void getOperation(GetOperationCallback cb);

  private:
    S21DataLink& m_dataLink;
};
