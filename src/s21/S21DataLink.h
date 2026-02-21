/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#pragma once

#include <cstddef>
#include <vector>

class S21DataLink {
public:
    virtual ~S21DataLink() = default;
    virtual void encodeAndTransmit(std::vector<std::byte> command) = 0;
    virtual std::vector<std::byte> receiveAndDecode() = 0;
};
