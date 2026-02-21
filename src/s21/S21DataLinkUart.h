/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#pragma once

#include "S21DataLink.h"

class S21DataLinkUart : public S21DataLink {
public:
    S21DataLinkUart(const struct device* uartDev) : m_uartDev(uartDev) {};

    void encodeAndTransmit(std::vector<std::byte> command) override;
    std::vector<std::byte> receiveAndDecode() override;

private:
    const struct device* m_uartDev;
};
