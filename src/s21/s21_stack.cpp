/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#include "s21_stack.h"
#include "s21_pinconfig.h"

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app, CONFIG_MATTER_LOG_LEVEL);

static constexpr auto kS21CacheMaxAge = std::chrono::seconds{10};

S21Stack::S21Stack()
        : mDataLink(NRF_UARTE21)
        , mPresentation(mDataLink)
        , mSyncAdapter(mPresentation)
        , mManager(mSyncAdapter, kS21CacheMaxAge, [] {
              return S21Manager::TimePoint(std::chrono::milliseconds(k_uptime_get()));
          })
{
}

int S21Stack::Init()
{
    int err = mDataLink.init(s21_pinconfig::kTxPin, s21_pinconfig::kRxPin);
    if (err) {
        LOG_ERR("S21DataLinkUart init failed: %d", err);
        return err;
    }

    mManager.Init();
    return 0;
}
