/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#pragma once

#include "s21_presentation_sync.h"

/**
 * @brief Mock implementation of S21PresentationSync for unit testing.
 *
 * All methods return configurable results immediately (no blocking).
 * Call counts and last setter arguments are tracked for verification.
 */
class MockS21PresentationSync : public S21PresentationSync {
  public:
    using GetProtocolVersionResult              = S21Presentation::GetProtocolVersionResult;
    using GetOperationResult                    = S21Presentation::GetOperationResult;
    using GetCoarseTemperatureAndHumidityResult = S21Presentation::GetCoarseTemperatureAndHumidityResult;

    // ── Configurable return values ────────────────────────────────────────

    tl::expected<GetProtocolVersionResult, S21PresentationError>
        protocolVersionResult{{1, 0}};
    tl::expected<GetProtocolVersionResult, S21PresentationError>
        extProtocolVersionResult{tl::unexpected(S21PresentationError{"NAK"})};
    tl::expected<GetOperationResult, S21PresentationError>
        operationResult{std::make_tuple(false, OperatingMode::Cool, 2200, FanMode::Auto)};
    tl::expected<void, S21DataLinkError>
        setOperationResult{};
    tl::expected<int16_t, S21PresentationError>
        roomTempResult{2350};
    tl::expected<int16_t, S21PresentationError>
        outdoorTempResult{3000};
    tl::expected<uint8_t, S21PresentationError>
        humidityResult{50};
    tl::expected<GetCoarseTemperatureAndHumidityResult, S21PresentationError>
        coarseResult{std::make_tuple<int16_t, int16_t, uint8_t>(2350, 3000, 50)};
    tl::expected<FanMode, S21PresentationError>
        fanModeResult{FanMode::Auto};

    // ── Call counts ───────────────────────────────────────────────────────

    int getProtocolVersionCallCount    = 0;
    int getExtProtocolVersionCallCount = 0;
    int getOperationCallCount          = 0;
    int setOperationCallCount          = 0;
    int getRoomTempCallCount           = 0;
    int getOutdoorTempCallCount        = 0;
    int getHumidityCallCount           = 0;
    int getCoarseCallCount             = 0;
    int getFanModeCallCount            = 0;

    // ── Last setOperation arguments ───────────────────────────────────────

    bool          lastOnOff    = false;
    OperatingMode lastMode     = OperatingMode::Cool;
    int16_t       lastSetPoint = 2200;
    FanMode       lastFanMode  = FanMode::Auto;

    // ── S21PresentationSync overrides ─────────────────────────────────────

    tl::expected<void, S21DataLinkError>
    setOperation(bool o, OperatingMode m, int16_t sp, FanMode f) override
    {
        ++setOperationCallCount;
        lastOnOff = o; lastMode = m; lastSetPoint = sp; lastFanMode = f;
        return setOperationResult;
    }

    tl::expected<GetOperationResult, S21PresentationError>
    getOperation() override
    {
        ++getOperationCallCount;
        return operationResult;
    }

    tl::expected<int16_t, S21PresentationError>
    getRoomTemperature() override
    {
        ++getRoomTempCallCount;
        return roomTempResult;
    }

    tl::expected<int16_t, S21PresentationError>
    getOutdoorTemperature() override
    {
        ++getOutdoorTempCallCount;
        return outdoorTempResult;
    }

    tl::expected<uint8_t, S21PresentationError>
    getHumidity() override
    {
        ++getHumidityCallCount;
        return humidityResult;
    }

    tl::expected<GetCoarseTemperatureAndHumidityResult, S21PresentationError>
    getCoarseTemperatureAndHumidity() override
    {
        ++getCoarseCallCount;
        return coarseResult;
    }

    tl::expected<FanMode, S21PresentationError>
    getFanMode() override
    {
        ++getFanModeCallCount;
        return fanModeResult;
    }

    tl::expected<GetProtocolVersionResult, S21PresentationError>
    getProtocolVersion() override
    {
        ++getProtocolVersionCallCount;
        return protocolVersionResult;
    }

    tl::expected<GetProtocolVersionResult, S21PresentationError>
    getExtendedProtocolVersion() override
    {
        ++getExtProtocolVersionCallCount;
        return extProtocolVersionResult;
    }
};
