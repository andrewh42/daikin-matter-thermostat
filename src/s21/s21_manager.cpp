/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#include "s21_manager.h"
#include <cerrno>

#ifdef __ZEPHYR__
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(s21_manager, LOG_LEVEL_DBG);
#endif

// ── CommandCapability ─────────────────────────────────────────────────────

bool S21Manager::CommandCapability::isUnsupported() const
{
    return status == CommandStatus::Unsupported;
}

void S21Manager::CommandCapability::recordSuccess()
{
    status       = CommandStatus::Supported;
    failureCount = 0;
}

void S21Manager::CommandCapability::recordFailure()
{
    if (status == CommandStatus::Supported) return; // once supported, stays supported
    if (++failureCount > kThreshold) status = CommandStatus::Unsupported;
}

// ── ReadThroughCache ──────────────────────────────────────────────────────

template<typename CachedType>
tl::expected<CachedType, S21PresentationError> S21Manager::ReadThroughCache<CachedType>::get()
{
    auto now = mClock();
    if (mValue && (now - mTimestamp) <= mMaxAge) {
#ifdef __ZEPHYR__
        LOG_DBG("%s cache: hit", mName);
#endif
        return *mValue;
    }
    auto result = mFetch();
    if (result) {
#ifdef __ZEPHYR__
        LOG_DBG("%s cache: miss, fetch succeeded", mName);
#endif
        mValue = *result; mTimestamp = now;
    } else {
#ifdef __ZEPHYR__
        LOG_DBG("%s cache: miss, fetch failed: %s", mName, result.error().what());
#endif
    }
    return result;
}

template tl::expected<S21Presentation::GetOperationResult, S21PresentationError>
    S21Manager::ReadThroughCache<S21Presentation::GetOperationResult>::get();
template tl::expected<S21Presentation::GetCoarseTemperatureAndHumidityResult, S21PresentationError>
    S21Manager::ReadThroughCache<S21Presentation::GetCoarseTemperatureAndHumidityResult>::get();

// ── S21Manager ────────────────────────────────────────────────────────────

S21Manager::S21Manager(S21PresentationSync& presentation,
                       std::chrono::seconds cacheMaxAge,
                       TimeSource timeSource)
        : mPresentation(presentation)
        , mOperationCache("operation", cacheMaxAge, timeSource,
                          [this] { return mPresentation.getOperation(); })
        , mCoarseTemperatureAndHumidityCache(
                  "coarse_temp_humidity", cacheMaxAge, timeSource, [this] {
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
    if (!version) {
#ifdef __ZEPHYR__
        LOG_WRN("S21 protocol version detection failed: %s", version.error().what());
#endif
        return -ENODEV;
    }

    mProtocolMajor = version->first;
#ifdef __ZEPHYR__
    LOG_DBG("protocol version: %u.%u", version->first, version->second);
#endif

    if (mProtocolMajor >= 2) {
        auto extVersion = mPresentation.getExtendedProtocolVersion();
        if (extVersion) {
            mProtocolMajor = extVersion->first;
#ifdef __ZEPHYR__
            LOG_DBG("extended protocol version: %u.%u", extVersion->first, extVersion->second);
#endif
        }
        else {
#ifdef __ZEPHYR__
            LOG_DBG("getExtendedProtocolVersion failed: %s", extVersion.error().what());
#endif
        }
    }

    mIsReady = true;
#ifdef __ZEPHYR__
    LOG_DBG("init complete, protocol major: %u", mProtocolMajor);
#endif
    return 0;
}

// ── Getters from operation cache ──────────────────────────────────────────

auto S21Manager::getOperation() -> Result<S21Presentation::GetOperationResult>
{
    if (!isReady()) return notReady();
    auto op = mOperationCache.get();
    if (!op) return presentationError(op.error());
    return *op;
}

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
