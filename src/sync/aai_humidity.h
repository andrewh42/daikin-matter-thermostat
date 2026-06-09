/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#pragma once

#include <app/AttributeAccessInterface.h>
#include <app-common/zap-generated/cluster-objects.h>

namespace sync { class SyncCoordinator; }

namespace sync_aai {

/**
 * HumidityBridgeAttributeAccess is the Matter AttributeAccessInterface
 * for the RelativeHumidityMeasurement cluster's `MeasuredValue` attribute.
 * The cluster server does not register its own AAI, so a plain subclass
 * plugs in without collision. Read-only: the cluster is sensor-output only.
 */
class HumidityBridgeAttributeAccess : public chip::app::AttributeAccessInterface {
public:
    HumidityBridgeAttributeAccess()
        : AttributeAccessInterface(chip::Optional<chip::EndpointId>::Missing(),
                                   chip::app::Clusters::RelativeHumidityMeasurement::Id)
    {
    }

    void Bind(sync::SyncCoordinator* stack) { mStack = stack; }

    CHIP_ERROR Read(const chip::app::ConcreteReadAttributePath& path,
                    chip::app::AttributeValueEncoder& encoder) override;

private:
    sync::SyncCoordinator* mStack{nullptr};
};

} // namespace sync_aai
