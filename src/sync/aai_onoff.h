/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * OnOffBridgeAttributeAccess
 * --------------------------
 * AAI for the OnOff cluster's `OnOff` attribute. The OnOff cluster server
 * does not register its own AAI, so a plain subclass plugs in without
 * collision.
 */
#pragma once

#include <app/AttributeAccessInterface.h>
#include <app-common/zap-generated/cluster-objects.h>

namespace sync { class SyncStack; }

namespace sync_aai {

class OnOffBridgeAttributeAccess : public chip::app::AttributeAccessInterface {
public:
    OnOffBridgeAttributeAccess()
        : AttributeAccessInterface(chip::Optional<chip::EndpointId>::Missing(),
                                   chip::app::Clusters::OnOff::Id)
    {
    }

    void Bind(sync::SyncStack* stack) { mStack = stack; }

    CHIP_ERROR Read(const chip::app::ConcreteReadAttributePath& path,
                    chip::app::AttributeValueEncoder& encoder) override;

    CHIP_ERROR Write(const chip::app::ConcreteDataAttributePath& path,
                     chip::app::AttributeValueDecoder& decoder) override;

private:
    sync::SyncStack* mStack{nullptr};
};

} // namespace sync_aai
