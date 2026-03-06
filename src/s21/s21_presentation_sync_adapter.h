/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#pragma once

#include "s21_presentation_sync.h"

#include <zephyr/kernel.h>

/**
 * @brief Synchronous adapter wrapping S21Presentation.
 *
 * Implements S21PresentationSync by calling the underlying async S21Presentation methods
 * and blocking the calling thread using a k_mutex + k_sem until the callback fires.
 *
 * Concurrent callers block at the mutex, ensuring at most one operation is in flight at a time.
 * This prevents busy errors from the underlying S21DataLink layer.
 *
 * Must not be called from the system work queue thread.
 */
class S21PresentationSyncAdapter : public S21PresentationSync {
  public:
    explicit S21PresentationSyncAdapter(S21Presentation& presentation);

    tl::expected<void, S21DataLinkError>
    setOperation(bool onOff, OperatingMode mode, int16_t setPoint, FanMode fanMode) override;

    tl::expected<S21Presentation::GetOperationResult, S21PresentationError>
    getOperation() override;

    tl::expected<S21Presentation::GetTemperatureResult, S21PresentationError>
    getRoomTemperature() override;

    tl::expected<S21Presentation::GetTemperatureResult, S21PresentationError>
    getOutdoorTemperature() override;

    tl::expected<S21Presentation::GetHumidityResult, S21PresentationError>
    getHumidity() override;

    tl::expected<S21Presentation::GetCoarseTemperatureAndHumidityResult, S21PresentationError>
    getCoarseTemperatureAndHumidity() override;

    tl::expected<S21Presentation::GetFanModeResult, S21PresentationError>
    getFanMode() override;

    tl::expected<S21Presentation::GetProtocolVersionResult, S21PresentationError>
    getProtocolVersion() override;

    tl::expected<S21Presentation::GetProtocolVersionResult, S21PresentationError>
    getExtendedProtocolVersion() override;

  private:
    template <typename T, typename E, typename AsyncFn>
    tl::expected<T, E> syncCall(AsyncFn&& fn);

    S21Presentation& mPresentation;
    struct k_mutex mMutex; // serialises concurrent callers
    struct k_sem mSem;     // signals completion of async callback
};
