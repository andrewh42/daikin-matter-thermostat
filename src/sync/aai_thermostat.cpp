/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#include "aai_thermostat.h"

#include "sync_reader.h"
#include "sync_stack.h"
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
    const auto& r = mStack->Reader();
    switch (path.mAttributeId) {
    case TAttr::LocalTemperature::Id:
        return encoder.Encode(r.ReadLocalTemperature());
    case TAttr::OutdoorTemperature::Id:
        return encoder.Encode(r.ReadOutdoorTemperature());
    case TAttr::OccupiedHeatingSetpoint::Id:
        return encoder.Encode(r.ReadOccupiedHeatingSetpoint());
    case TAttr::OccupiedCoolingSetpoint::Id:
        return encoder.Encode(r.ReadOccupiedCoolingSetpoint());
    case TAttr::SystemMode::Id:
        return encoder.Encode(r.ReadSystemMode());
    case TAttr::ThermostatRunningMode::Id:
        return encoder.Encode(r.ReadRunningMode());
    case TAttr::SetpointChangeSource::Id:
        return encoder.Encode(r.ReadSetpointChangeSource());
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
        mStack->ApplyIntent(sync::SetSystemModeIntent{v});
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
