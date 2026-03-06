/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#pragma once

#include "s21_presentation.h"

/**
 * @brief Synchronous (blocking) abstract interface for the S21 presentation layer.
 *
 * Mirrors all S21Presentation methods but blocks the calling thread until the operation
 * completes, returning the result directly instead of taking a callback.
 *
 * Must not be called from the system work queue thread, as the underlying async
 * implementation delivers results there.
 */
class S21PresentationSync {
  public:
    virtual ~S21PresentationSync() = default;

    /// @brief Sets the operation mode. Blocks until the AC acknowledges the command.
    /// @param onOff true for ON, false for OFF.
    /// @param mode cooling/heating mode.
    /// @param setPoint temperature set point in hundredths of Celsius degrees (e.g. 2050 => 20.50°C).
    /// @param fanMode fan operating mode.
    virtual tl::expected<void, S21DataLinkError>
    setOperation(bool onOff, OperatingMode mode, int16_t setPoint, FanMode fanMode) = 0;

    /// @brief Gets the current operation mode. Blocks until the response is received.
    /// @return (onOff, mode, setPoint, fanMode) on success, or an error.
    virtual tl::expected<S21Presentation::GetOperationResult, S21PresentationError>
    getOperation() = 0;

    /// @brief Reads the room (indoor) temperature. Blocks until the response is received.
    /// @return temperature in 0.01 °C units on success, or an error.
    virtual tl::expected<S21Presentation::GetTemperatureResult, S21PresentationError>
    getRoomTemperature() = 0;

    /// @brief Reads the outdoor temperature. Blocks until the response is received.
    /// @return temperature in 0.01 °C units on success, or an error.
    virtual tl::expected<S21Presentation::GetTemperatureResult, S21PresentationError>
    getOutdoorTemperature() = 0;

    /// @brief Reads the indoor relative humidity. Blocks until the response is received.
    /// @return humidity percentage (0–100) on success, or an error.
    virtual tl::expected<S21Presentation::GetHumidityResult, S21PresentationError>
    getHumidity() = 0;

    /// @brief Reads coarse indoor/outdoor temperatures and humidity. Blocks until the response is received.
    /// @return (indoorTemp, outdoorTemp, humidity) on success, or an error.
    virtual tl::expected<S21Presentation::GetCoarseTemperatureAndHumidityResult, S21PresentationError>
    getCoarseTemperatureAndHumidity() = 0;

    /// @brief Reads the current fan mode. Blocks until the response is received.
    /// @return FanMode on success, or an error.
    virtual tl::expected<S21Presentation::GetFanModeResult, S21PresentationError>
    getFanMode() = 0;

    /// @brief Reads protocol version via the legacy F8 command. Blocks until the response is received.
    /// @return {major, minor} on success, or an error.
    virtual tl::expected<S21Presentation::GetProtocolVersionResult, S21PresentationError>
    getProtocolVersion() = 0;

    /// @brief Reads protocol version via the FY00 command (v3+ units only). Blocks until the response is received.
    /// @return {major, minor} on success, or an error if the unit sends NAK.
    virtual tl::expected<S21Presentation::GetProtocolVersionResult, S21PresentationError>
    getExtendedProtocolVersion() = 0;
};
