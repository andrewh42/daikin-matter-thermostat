/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

#include "S21DataLinkUart.h"

void S21DataLinkUart::encodeAndTransmit(std::vector<std::byte> payload)
{
}

tl::expected<std::vector<std::byte>, S21DataLinkError> S21DataLinkUart::receiveAndDecode()
{
    return tl::unexpected(S21DataLinkError("not implemented"));
}
