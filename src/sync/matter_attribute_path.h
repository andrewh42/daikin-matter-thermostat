/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Lightweight (endpoint, cluster, attribute) triple used by the reconciler
 * to communicate which cluster attributes have changed and should be
 * reported to subscribers. Mirrors chip::app::ConcreteAttributePath but
 * stays free of CHIP headers so host tests work.
 */
#pragma once

#include <app-common/zap-generated/attributes/Accessors.h>

#include <cstdint>

namespace sync {

struct MatterAttributePath {
    chip::EndpointId  endpoint;
    chip::ClusterId   cluster;
    chip::AttributeId attribute;

    bool operator==(const MatterAttributePath& o) const
    {
        return endpoint == o.endpoint && cluster == o.cluster && attribute == o.attribute;
    }
    bool operator!=(const MatterAttributePath& o) const { return !(*this == o); }
};

} // namespace sync
