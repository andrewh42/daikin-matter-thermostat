/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#include "s21_manager.h"

S21Manager::S21Manager(S21PresentationSync& presentation,
                       std::chrono::seconds cacheMaxAge,
                       TimeSource timeSource)
        : mPresentation(presentation)
        , mOperationCache(cacheMaxAge, timeSource,
                          [this] { return mPresentation.getOperation(); })
        , mCoarseTemperatureAndHumidityCache(
                  cacheMaxAge, timeSource, [this] {
                      if (mCoarseTemperatureAndHumidityCapability.isUnsupported())
                          return tl::expected<S21Presentation::GetCoarseTemperatureAndHumidityResult,
                                              S21PresentationError>{
                              tl::unexpected(S21PresentationError{"unsupported"})};
                      auto r = mPresentation.getCoarseTemperatureAndHumidity();
                      mCoarseTemperatureAndHumidityCapability.record(r);
                      return r;
                  })
{}

int S21Manager::Init()
{
    if (mIsReady) return 0;

    auto version = mPresentation.getProtocolVersion();
    if (version) mProtocolMajor = version->first;

    if (mProtocolMajor >= 2) {
        auto extVersion = mPresentation.getExtendedProtocolVersion();
        if (extVersion) mProtocolMajor = extVersion->first; // NAK → ignored
    }

    mIsReady = true;
    return 0;
}

// ── Getters from operation cache ──────────────────────────────────────────

auto S21Manager::getOnOff() -> Result<bool>
{
    if (!isReady()) return notReady();
    auto op = mOperationCache.get();
    if (!op) return presentationError(op.error());
    return std::get<0>(*op);
}

auto S21Manager::getOperatingMode() -> Result<OperatingMode>
{
    if (!isReady()) return notReady();
    auto op = mOperationCache.get();
    if (!op) return presentationError(op.error());
    return std::get<1>(*op);
}

auto S21Manager::getSetpoint() -> Result<int16_t>
{
    if (!isReady()) return notReady();
    auto op = mOperationCache.get();
    if (!op) return presentationError(op.error());
    return std::get<2>(*op);
}

// ── Getters with fallback ─────────────────────────────────────────────────

auto S21Manager::getFanMode() -> Result<FanMode>
{
    if (!isReady()) return notReady();
    auto result = mFanModeCapability.callWithTracking(
        [this] { return mPresentation.getFanMode(); });
    if (result) return *result;
    auto op = mOperationCache.get();
    if (!op) return presentationError(op.error());
    return std::get<3>(*op);
}

auto S21Manager::getRoomTemperature() -> Result<int16_t>
{
    if (!isReady()) return notReady();
    auto result = mRoomTemperatureCapability.callWithTracking(
        [this] { return mPresentation.getRoomTemperature(); });
    if (result) return *result;
    auto coarse = mCoarseTemperatureAndHumidityCache.get();
    if (!coarse) return presentationError(coarse.error());
    return std::get<0>(*coarse);
}

auto S21Manager::getOutdoorTemperature() -> Result<int16_t>
{
    if (!isReady()) return notReady();
    auto result = mOutdoorTemperatureCapability.callWithTracking(
        [this] { return mPresentation.getOutdoorTemperature(); });
    if (result) return *result;
    auto coarse = mCoarseTemperatureAndHumidityCache.get();
    if (!coarse) return presentationError(coarse.error());
    return std::get<1>(*coarse);
}

auto S21Manager::getHumidity() -> Result<uint8_t>
{
    if (!isReady()) return notReady();
    auto result = mHumidityCapability.callWithTracking(
        [this] { return mPresentation.getHumidity(); });
    if (result) return *result;
    auto coarse = mCoarseTemperatureAndHumidityCache.get();
    if (!coarse) return presentationError(coarse.error());
    return std::get<2>(*coarse);
}

// ── Write operations ──────────────────────────────────────────────────────

auto S21Manager::setOperation(bool onOff, OperatingMode mode,
                              int16_t setPoint, FanMode fanMode) -> Result<void>
{
    if (!isReady()) return notReady();
    auto result = mPresentation.setOperation(onOff, mode, setPoint, fanMode);
    if (!result) return dataLinkError(result.error());
    mOperationCache.update(std::make_tuple(onOff, mode, setPoint, fanMode));
    return {};
}

auto S21Manager::setOnOff(bool onOff) -> Result<void>
{
    if (!isReady()) return notReady();
    auto op = mOperationCache.get();
    if (!op) return presentationError(op.error());
    return setOperation(onOff, std::get<1>(*op), std::get<2>(*op), std::get<3>(*op));
}

auto S21Manager::setSetpoint(int16_t setPoint) -> Result<void>
{
    if (!isReady()) return notReady();
    auto op = mOperationCache.get();
    if (!op) return presentationError(op.error());
    return setOperation(std::get<0>(*op), std::get<1>(*op), setPoint, std::get<3>(*op));
}

auto S21Manager::setFanMode(FanMode fanMode) -> Result<void>
{
    if (!isReady()) return notReady();
    auto op = mOperationCache.get();
    if (!op) return presentationError(op.error());
    return setOperation(std::get<0>(*op), std::get<1>(*op), std::get<2>(*op), fanMode);
}
