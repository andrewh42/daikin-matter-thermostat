/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#pragma once

#include <tl/expected.hpp>
#include <cstddef>
#include <stdexcept>
#include <vector>

struct S21DataLinkError: std::runtime_error {
    using std::runtime_error::runtime_error;
};

class S21DataLink {
  public:
    virtual ~S21DataLink() = default;
    virtual void encodeAndTransmit(std::vector<std::byte> payload) = 0;
    virtual tl::expected<std::vector<std::byte>, S21DataLinkError> receiveAndDecode() = 0;
};
