/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#pragma once

#include <tl/expected.hpp>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <vector>

struct S21DataLinkError: std::runtime_error {
    using std::runtime_error::runtime_error;
};

/**
 * S21DataLink is the abstract data-link layer for the Daikin S21 bus.
 * Encodes outbound payloads into framed S21 packets, transmits them, and
 * delivers ACK/NAK or response frames back to the caller via the supplied
 * callback. Implementations choose the transport (UART, mock, etc.).
 */
class S21DataLink {
  public:
    virtual ~S21DataLink() = default;

    using SendCallback = std::function<void(tl::expected<void, S21DataLinkError>)>;
    using TransactCallback = std::function<void(tl::expected<std::vector<std::byte>, S21DataLinkError>)>;

    /// Encode payload, transmit, wait for ACK/NAK. Callback with success or error.
    virtual void send(std::vector<std::byte> payload, SendCallback cb) = 0;

    /// Encode payload, transmit, wait for ACK + response frame.
    /// Callback with decoded response payload or error.
    virtual void transact(std::vector<std::byte> payload, TransactCallback cb) = 0;
};
