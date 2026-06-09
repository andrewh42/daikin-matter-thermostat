/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#pragma once

#include "s21_datalink_uart.h"
#include "s21_manager.h"
#include "s21_presentation.h"
#include "s21_presentation_sync_adapter.h"

/**
 * S21Stack is the production singleton that owns the full S21 transport
 * stack — the UARTE data link, the async S21Presentation built on top of
 * it, the synchronous adapter that wraps the presentation for non-work-
 * queue callers, and the S21Manager orchestration layer above the
 * adapter. Init() wires the layers together and brings them up in order.
 */
class S21Stack {
  public:
    static S21Stack& Instance()
    {
        static S21Stack sS21Stack;
        return sS21Stack;
    };

    S21Stack();
    ~S21Stack() = default;

    int Init();

    S21Presentation& GetPresentation()
    {
        return mPresentation;
    }

    S21Manager& GetManager()
    {
        return mManager;
    }

  private:
    S21DataLinkUart            mDataLink;
    S21Presentation            mPresentation;
    S21PresentationSyncAdapter mSyncAdapter;
    S21Manager                 mManager;
};
