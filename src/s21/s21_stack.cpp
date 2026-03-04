/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#include "s21_stack.h"
#include "s21_pinconfig.h"

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app, CONFIG_MATTER_LOG_LEVEL);

S21Stack::S21Stack()
        : mDataLink(NRF_UARTE21)
        , mPresentation(mDataLink)
{
}

int S21Stack::Init()
{
    int err = mDataLink.init(s21_pinconfig::kTxPin, s21_pinconfig::kRxPin);
    if (err) {
        LOG_ERR("S21DataLinkUart init failed: %d", err);
        return err;
    }

    return 0;
}
