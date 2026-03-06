/**
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#include "s21_datalink_uart.h"
#include "s21_manager.h"
#include "s21_presentation.h"
#include "s21_presentation_sync_adapter.h"

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
