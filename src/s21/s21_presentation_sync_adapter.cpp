/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#include "s21_presentation_sync_adapter.h"

#include <optional>

S21PresentationSyncAdapter::S21PresentationSyncAdapter(S21Presentation& presentation)
        : mPresentation(presentation)
{
    k_mutex_init(&mMutex);
    k_sem_init(&mSem, 0, 1);
}

template <typename T, typename E, typename AsyncFn>
tl::expected<T, E> S21PresentationSyncAdapter::syncCall(AsyncFn&& fn)
{
    k_mutex_lock(&mMutex, K_FOREVER);
    std::optional<tl::expected<T, E>> result;
    fn([&](tl::expected<T, E> r) {
        result = std::move(r);
        k_sem_give(&mSem);
    });
    k_sem_take(&mSem, K_FOREVER);
    k_mutex_unlock(&mMutex);
    __ASSERT(result.has_value(), "S21 async callback did not fire");
    return std::move(*result);
}

tl::expected<void, S21DataLinkError>
S21PresentationSyncAdapter::setOperation(bool onOff, OperatingMode mode, int16_t setPoint, FanMode fanMode)
{
    return syncCall<void, S21DataLinkError>(
            [&](auto cb) { mPresentation.setOperation(onOff, mode, setPoint, fanMode, std::move(cb)); });
}

tl::expected<S21Presentation::GetOperationResult, S21PresentationError>
S21PresentationSyncAdapter::getOperation()
{
    return syncCall<S21Presentation::GetOperationResult, S21PresentationError>(
            [&](auto cb) { mPresentation.getOperation(std::move(cb)); });
}

tl::expected<S21Presentation::GetTemperatureResult, S21PresentationError>
S21PresentationSyncAdapter::getRoomTemperature()
{
    return syncCall<S21Presentation::GetTemperatureResult, S21PresentationError>(
            [&](auto cb) { mPresentation.getRoomTemperature(std::move(cb)); });
}

tl::expected<S21Presentation::GetTemperatureResult, S21PresentationError>
S21PresentationSyncAdapter::getOutdoorTemperature()
{
    return syncCall<S21Presentation::GetTemperatureResult, S21PresentationError>(
            [&](auto cb) { mPresentation.getOutdoorTemperature(std::move(cb)); });
}

tl::expected<S21Presentation::GetHumidityResult, S21PresentationError>
S21PresentationSyncAdapter::getHumidity()
{
    return syncCall<S21Presentation::GetHumidityResult, S21PresentationError>(
            [&](auto cb) { mPresentation.getHumidity(std::move(cb)); });
}

tl::expected<S21Presentation::GetCoarseTemperatureAndHumidityResult, S21PresentationError>
S21PresentationSyncAdapter::getCoarseTemperatureAndHumidity()
{
    return syncCall<S21Presentation::GetCoarseTemperatureAndHumidityResult, S21PresentationError>(
            [&](auto cb) { mPresentation.getCoarseTemperatureAndHumidity(std::move(cb)); });
}

tl::expected<S21Presentation::GetFanModeResult, S21PresentationError>
S21PresentationSyncAdapter::getFanMode()
{
    return syncCall<S21Presentation::GetFanModeResult, S21PresentationError>(
            [&](auto cb) { mPresentation.getFanMode(std::move(cb)); });
}

tl::expected<S21Presentation::GetProtocolVersionResult, S21PresentationError>
S21PresentationSyncAdapter::getProtocolVersion()
{
    return syncCall<S21Presentation::GetProtocolVersionResult, S21PresentationError>(
            [&](auto cb) { mPresentation.getProtocolVersion(std::move(cb)); });
}

tl::expected<S21Presentation::GetProtocolVersionResult, S21PresentationError>
S21PresentationSyncAdapter::getExtendedProtocolVersion()
{
    return syncCall<S21Presentation::GetProtocolVersionResult, S21PresentationError>(
            [&](auto cb) { mPresentation.getExtendedProtocolVersion(std::move(cb)); });
}
