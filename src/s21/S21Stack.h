/**
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#include "S21DataLinkUart.h"
#include "S21Presentation.h"

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

  private:
    S21DataLinkUart mDataLink;
    S21Presentation mPresentation;
};
