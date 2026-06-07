/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * ThermostatBridgeAttributeAccess
 * -------------------------------
 * AAI for the Thermostat cluster. Installed in place of the SDK's
 * gThermostatAttrAccess after Unregister; see sync-option-4-plan Phase 5.
 *
 * Handles the externalised attributes:
 *   LocalTemperature, OutdoorTemperature, OccupiedHeatingSetpoint,
 *   OccupiedCoolingSetpoint, SystemMode, ThermostatRunningMode,
 *   SetpointChangeSource[Timestamp].
 *
 * Any other attribute (FeatureMap, MinSetpointDeadBand, Presets*,
 * Schedule*, …) falls through to cluster-server / RAM storage by
 * returning CHIP_NO_ERROR without encoding.
 */
#pragma once

#include <app/AttributeAccessInterface.h>
#include <app-common/zap-generated/cluster-objects.h>

namespace sync { class SyncStack; }

namespace sync_aai {

class ThermostatBridgeAttributeAccess : public chip::app::AttributeAccessInterface {
public:
    ThermostatBridgeAttributeAccess()
        : AttributeAccessInterface(chip::Optional<chip::EndpointId>::Missing(),
                                   chip::app::Clusters::Thermostat::Id)
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
