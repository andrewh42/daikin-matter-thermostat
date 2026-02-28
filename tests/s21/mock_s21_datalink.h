/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#pragma once

#include "S21DataLink.h"
#include <optional>
#include <vector>

class MockS21DataLink : public S21DataLink {
public:
    std::vector<std::byte> lastTransmitted;
    std::vector<std::byte> nextResponse;
    std::optional<S21DataLinkError> nextError;

    void send(std::vector<std::byte> payload, SendCallback cb) override
    {
        lastTransmitted = std::move(payload);
        if (nextError) {
            cb(tl::unexpected(*nextError));
        } else {
            cb({});
        }
    }

    void transact(std::vector<std::byte> payload, TransactCallback cb) override
    {
        lastTransmitted = std::move(payload);
        if (nextError) {
            cb(tl::unexpected(*nextError));
        } else {
            cb(nextResponse);
        }
    }
};
