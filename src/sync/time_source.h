/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * TimeSource
 * ----------
 * The reconciler and atomic buffer make policy decisions that depend on
 * elapsed time (guard windows, atomic-transaction timeouts). Injecting a
 * TimeSource keeps that code host-testable: production wires the Zephyr
 * monotonic clock, tests wire a manual clock they can step.
 */
#pragma once

#include <cstdint>

namespace sync {

class TimeSource {
public:
    virtual ~TimeSource() = default;
    /// Monotonic time in milliseconds. Need not be wall-clock.
    virtual int64_t millis() const = 0;
};

/// Trivial time source for tests; step with advance().
class ManualTimeSource : public TimeSource {
public:
    explicit ManualTimeSource(int64_t initial = 0) : mNow(initial) {}
    int64_t millis() const override { return mNow; }
    void    advance(int64_t ms)    { mNow += ms; }
    void    set(int64_t ms)        { mNow = ms; }
private:
    int64_t mNow;
};

} // namespace sync
