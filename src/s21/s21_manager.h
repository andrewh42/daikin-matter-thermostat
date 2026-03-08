/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#pragma once

#include "s21_presentation_sync.h"

#include <chrono>
#include <functional>
#include <optional>

/**
 * @brief Orchestration layer above S21PresentationSync.
 *
 * Responsibilities:
 *  - Init sequence: detect S21 protocol version.
 *  - Per-command capability tracking: marks commands unsupported after
 *    repeated failure, enabling graceful fallback to lower-precision alternatives.
 *  - Read-through caching of getOperation and getCoarseTemperatureAndHumidity results,
 *    with configurable max-age.
 *  - Individual get/set accessors built on top of S21PresentationSync.
 *
 * Threading: All methods must be called from the same non-work-queue thread.
 * No Zephyr APIs are used. Must not be called from the system work queue thread
 * (same restriction as the underlying S21PresentationSync implementation).
 */
class S21Manager {
  public:
    using Clock      = std::chrono::steady_clock;
    using TimePoint  = Clock::time_point;
    using TimeSource = std::function<TimePoint()>;

    static TimeSource defaultTimeSource()
    {
        return [] { return Clock::now(); };
    }

    struct Error {
        const char* message;
        explicit Error(const char* msg) : message(msg) {}
    };

    template<typename T>
    using Result = tl::expected<T, Error>;

    /**
     * @param presentation  S21PresentationSync to use (must outlive this object).
     * @param cacheMaxAge   How long cached results remain valid. Default 10 seconds.
     * @param timeSource    Injectable clock for testing. Default: steady_clock::now.
     */
    explicit S21Manager(S21PresentationSync& presentation,
                        std::chrono::seconds cacheMaxAge = std::chrono::seconds{10},
                        TimeSource timeSource = defaultTimeSource());

    S21Manager(const S21Manager&)            = delete;
    S21Manager& operator=(const S21Manager&) = delete;

    /**
     * @brief Run the init sequence synchronously (query protocol version).
     * Idempotent — subsequent calls are no-ops.
     * @return 0 on success.
     */
    int Init();

    bool isReady() const { return mIsReady; }

    // ── Read operations ───────────────────────────────────────────────────

    /// Returns the full operation result (on/off, mode, setpoint, fan mode) from cache.
    Result<S21Presentation::GetOperationResult> getOperation();

    /// Returns on/off state from the cached getOperation result.
    Result<bool> getOnOff();

    /// Returns operating mode from the cached getOperation result.
    Result<OperatingMode> getOperatingMode();

    /// Returns temperature setpoint (0.01 °C) from the cached getOperation result.
    Result<int16_t> getSetpoint();

    /// Returns fan mode. Tries RG command; falls back to getOperation cache on failure.
    Result<FanMode> getFanMode();

    /// Returns indoor temperature (0.01 °C). Tries RH; falls back to F9 indoor on failure.
    Result<int16_t> getRoomTemperature();

    /// Returns outdoor temperature (0.01 °C). Tries Ra; falls back to F9 outdoor on failure.
    Result<int16_t> getOutdoorTemperature();

    /// Returns indoor humidity (%). Tries Re; falls back to F9 humidity on failure.
    Result<uint8_t> getHumidity();

    // ── Write operations ──────────────────────────────────────────────────

    /// Full set operation — mirrors S21Presentation::setOperation.
    /// Updates the operation cache on success.
    Result<void> setOperation(bool onOff, OperatingMode mode, int16_t setPoint, FanMode fanMode);

    /// Sets on/off state only. Uses (and auto-refreshes if stale) the operation cache.
    Result<void> setOnOff(bool onOff);

    /// Sets temperature setpoint only. Uses (and auto-refreshes if stale) the operation cache.
    Result<void> setSetpoint(int16_t setPoint);

    /// Sets fan mode only. Uses (and auto-refreshes if stale) the operation cache.
    Result<void> setFanMode(FanMode fanMode);

  private:
    // ── CommandCapability ─────────────────────────────────────────────────

    enum class CommandStatus { Unknown, Supported, Unsupported };

    struct CommandCapability {
        CommandStatus status       = CommandStatus::Unknown;
        int           failureCount = 0;
        static constexpr int kThreshold = 3;

        bool isUnsupported() const;
        void recordSuccess();
        void recordFailure();

        template<typename T, typename E>
        void record(const tl::expected<T, E>& result)
        {
            if (result) recordSuccess();
            else        recordFailure();
        }

        /// If Unsupported, returns error without calling fetch.
        /// Otherwise calls fetch, records the result, and returns it.
        template<typename FetchFn>
        auto callWithTracking(FetchFn fetch) -> decltype(fetch())
        {
            if (isUnsupported()) return tl::unexpected(S21PresentationError{"unsupported"});
            auto result = fetch();
            record(result);
            return result;
        }
    };

    // ── ReadThroughCache ──────────────────────────────────────────────────

    template<typename CachedType>
    class ReadThroughCache {
      public:
        using FetchFn = std::function<tl::expected<CachedType, S21PresentationError>()>;

        ReadThroughCache(const char* name, std::chrono::seconds maxAge, TimeSource clock, FetchFn fetch)
            : mName(name), mMaxAge(maxAge), mClock(std::move(clock)), mFetch(std::move(fetch))
        {}

        /// Returns cached value if fresh; otherwise calls FetchFn and caches on success.
        tl::expected<CachedType, S21PresentationError> get();

        /// Directly write a value, timestamping with the internal clock.
        void update(CachedType val)
        {
            mValue     = std::move(val);
            mTimestamp = mClock();
        }

        void invalidate() { mValue.reset(); }

        const std::optional<CachedType>& value() const { return mValue; }

      private:
        const char*             mName;
        std::chrono::seconds    mMaxAge;
        TimeSource              mClock;
        FetchFn                 mFetch;
        std::optional<CachedType> mValue;
        TimePoint               mTimestamp{};
    };

    // ── Error helpers ─────────────────────────────────────────────────────

    static tl::unexpected<Error> notReady()
    {
        return tl::unexpected(Error{"not ready"});
    }

    static tl::unexpected<Error> presentationError(const S21PresentationError& e)
    {
        return tl::unexpected(Error{e.what()});
    }

    static tl::unexpected<Error> dataLinkError(const S21DataLinkError& e)
    {
        return tl::unexpected(Error{e.what()});
    }

    // ── Members ───────────────────────────────────────────────────────────

    S21PresentationSync& mPresentation;
    bool                 mIsReady{false};
    uint8_t              mProtocolMajor{0};

    CommandCapability mRoomTemperatureCapability;
    CommandCapability mOutdoorTemperatureCapability;
    CommandCapability mHumidityCapability;
    CommandCapability mCoarseTemperatureAndHumidityCapability;
    CommandCapability mFanModeCapability;

    // Caches declared after capability members so FetchFn lambdas that capture `this`
    // access fully-initialized members when get() is eventually called.
    ReadThroughCache<S21Presentation::GetOperationResult>                    mOperationCache;
    ReadThroughCache<S21Presentation::GetCoarseTemperatureAndHumidityResult> mCoarseTemperatureAndHumidityCache;
    ReadThroughCache<FanMode>                                                mFanModeCache;
};
