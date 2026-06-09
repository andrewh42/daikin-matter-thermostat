/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#pragma once

#include "time_source.h"

#include <zephyr/kernel.h>

namespace sync {

/**
 * MonotonicTimeSource is the production TimeSource backed by Zephyr's
 * monotonic uptime clock.
 */
class MonotonicTimeSource : public TimeSource {
public:
    int64_t millis() const override { return k_uptime_get(); }
};

} // namespace sync
