/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Production TimeSource backed by Zephyr's monotonic uptime clock.
 */
#pragma once

#include "time_source.h"

#include <zephyr/kernel.h>

namespace sync {

class MonotonicTimeSource : public TimeSource {
public:
    int64_t millis() const override { return k_uptime_get(); }
};

} // namespace sync
