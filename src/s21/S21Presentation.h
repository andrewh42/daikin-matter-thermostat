/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#pragma once

#include "S21DataLink.h"

#include <functional>
#include <stdint.h>
#include <tuple>
#include <utility>

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

    using GetTemperatureResult = int16_t; // 0.01 °C units
    using GetTemperatureCallback = std::function<void(tl::expected<GetTemperatureResult, S21DataLinkError>)>;

    using GetHumidityResult = uint8_t; // percentage 0–100
    using GetHumidityCallback = std::function<void(tl::expected<GetHumidityResult, S21DataLinkError>)>;

    /// @brief Reads the room (indoor) temperature from the AC sensor.
    /// @param cb callback invoked with temperature in 0.01 °C units on success, or an error.
    void getRoomTemperature(GetTemperatureCallback cb);

    /// @brief Reads the outdoor temperature from the AC sensor.
    /// @param cb callback invoked with temperature in 0.01 °C units on success, or an error.
    void getOutdoorTemperature(GetTemperatureCallback cb);

    /// @brief Reads the indoor relative humidity from the AC sensor.
    /// @param cb callback invoked with humidity percentage (0–100) on success, or an error.
    void getHumidity(GetHumidityCallback cb);

    using GetCoarseTemperatureAndHumidityResult = std::tuple<int16_t, int16_t, uint8_t>; // indoor, outdoor (0.01 °C), humidity (%)
    using GetCoarseTemperatureAndHumidityCallback = std::function<void(tl::expected<GetCoarseTemperatureAndHumidityResult, S21DataLinkError>)>;

    using GetFanModeResult = FanMode;
    using GetFanModeCallback = std::function<void(tl::expected<GetFanModeResult, S21DataLinkError>)>;

    using GetProtocolVersionResult = std::pair<uint8_t, uint8_t>; // {major, minor}
    using GetProtocolVersionCallback = std::function<void(tl::expected<GetProtocolVersionResult, S21DataLinkError>)>;

    /// @brief Reads coarse indoor/outdoor temperatures (0.01 °C, 0.5 °C steps) and humidity (%, 5% steps).
    /// @param cb callback invoked with (indoorTemp, outdoorTemp, humidity) on success, or an error.
    void getCoarseTemperatureAndHumidity(GetCoarseTemperatureAndHumidityCallback cb);

    /// @brief Reads the current fan mode via the RG command. ASCII encoding, same as G1.
    /// @param cb callback invoked with the current FanMode on success, or an error.
    void getFanMode(GetFanModeCallback cb);

    /// @brief Reads protocol version via the legacy F8 command (supported by all units).
    /// v3+ units respond with {major=2, minor=0}.
    /// @param cb callback invoked with {major, minor} on success.
    void getProtocolVersion(GetProtocolVersionCallback cb);

    /// @brief Reads protocol version via the FY00 command (v3+ units only).
    /// Older units respond with NAK, which is reported as an error.
    /// @param cb callback invoked with {major, minor} on success, or an error if the unit sends NAK.
    void getExtendedProtocolVersion(GetProtocolVersionCallback cb);

  private:
    S21DataLink& m_dataLink;
};
