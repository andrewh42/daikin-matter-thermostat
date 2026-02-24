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

    void send(std::vector<std::byte>, SendCallback) override {}
    void transact(std::vector<std::byte>, TransactCallback) override {}

    void encodeAndTransmit(std::vector<std::byte> command) override
    {
        lastTransmitted = std::move(command);
    }

    tl::expected<std::vector<std::byte>, S21DataLinkError> receiveAndDecode() override
    {
        if (nextError) {
            return tl::unexpected(*nextError);
        }
        return nextResponse;
    }
};
