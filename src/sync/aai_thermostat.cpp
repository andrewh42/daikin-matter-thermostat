/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#include "aai_thermostat.h"

#include "aai_translation.h"
#include "sync_coordinator.h"
#include "write_intent.h"

#include <app/AttributeAccessInterface.h>
#include <protocols/interaction_model/StatusCode.h>

namespace sync_aai {

using namespace chip;
using namespace chip::app;
namespace TAttr = Clusters::Thermostat::Attributes;
using SystemModeEnum = Clusters::Thermostat::SystemModeEnum;

CHIP_ERROR ThermostatBridgeAttributeAccess::Read(const ConcreteReadAttributePath& path,
                                                 AttributeValueEncoder& encoder)
{
    auto& r = *mStack;
    switch (path.mAttributeId) {
    case TAttr::LocalTemperature::Id:
        return encoder.Encode(sync_aai::wrap(r.ReadLocalTemperature()));
    case TAttr::OutdoorTemperature::Id:
        return encoder.Encode(sync_aai::wrap(r.ReadOutdoorTemperature()));
    case TAttr::OccupiedHeatingSetpoint::Id:
        return encoder.Encode(r.ReadOccupiedHeatingSetpoint());
    case TAttr::OccupiedCoolingSetpoint::Id:
        return encoder.Encode(r.ReadOccupiedCoolingSetpoint());
    case TAttr::SystemMode::Id:
        return encoder.Encode(sync_aai::toMatterSystemMode(r.ReadOnOff(), r.ReadMode()));
    case TAttr::ThermostatRunningMode::Id:
        return encoder.Encode(sync_aai::toMatterRunningMode(r.ReadRunningMode()));
    case TAttr::SetpointChangeSource::Id:
        return encoder.Encode(sync_aai::toMatterSetpointSource(r.ReadSetpointSource()));
    case TAttr::SetpointChangeSourceTimestamp::Id:
        // We could expose the active twin's lastObservationTime, but the
        // value is meaningful only if we maintain it in epoch seconds.
        // For now, leave nullable and let the cluster server's RAM value
        // win by returning success-without-encoding.
        return CHIP_NO_ERROR;
    default:
        return CHIP_NO_ERROR; // fallback for Presets, Schedules, FeatureMap, …
    }
}

CHIP_ERROR ThermostatBridgeAttributeAccess::Write(const ConcreteDataAttributePath& path,
                                                  AttributeValueDecoder& decoder)
{
    switch (path.mAttributeId) {
    case TAttr::OccupiedHeatingSetpoint::Id: {
        int16_t v;
        ReturnErrorOnFailure(decoder.Decode(v));
        mStack->ApplyIntent(sync::SetOccupiedHeatingSetpointIntent{v});
        return CHIP_NO_ERROR;
    }
    case TAttr::OccupiedCoolingSetpoint::Id: {
        int16_t v;
        ReturnErrorOnFailure(decoder.Decode(v));
        mStack->ApplyIntent(sync::SetOccupiedCoolingSetpointIntent{v});
        return CHIP_NO_ERROR;
    }
    case TAttr::SystemMode::Id: {
        SystemModeEnum v;
        ReturnErrorOnFailure(decoder.Decode(v));
        auto translated = sync_aai::fromMatterSystemMode(v);
        if (!translated.has_value()) return CHIP_IM_GLOBAL_STATUS(ConstraintError);
        mStack->ApplyIntent(sync::SetSystemModeIntent{translated->first, translated->second});
        return CHIP_NO_ERROR;
    }
    case TAttr::LocalTemperature::Id:
    case TAttr::OutdoorTemperature::Id:
    case TAttr::ThermostatRunningMode::Id:
    case TAttr::SetpointChangeSource::Id:
    case TAttr::SetpointChangeSourceTimestamp::Id:
        // Read-only from a controller perspective.
        return CHIP_IM_GLOBAL_STATUS(UnsupportedWrite);
    default:
        return CHIP_NO_ERROR; // fallback (Presets/Schedules edits)
    }
}

} // namespace sync_aai
