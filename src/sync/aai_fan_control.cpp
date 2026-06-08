/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#include "aai_fan_control.h"

#include "sync_reader.h"
#include "sync_stack.h"
#include "write_intent.h"

#include <app/AttributeAccessInterface.h>
#include <app/data-model/Nullable.h>

namespace sync_aai {

using namespace chip;
using namespace chip::app;
namespace FCAttr = Clusters::FanControl::Attributes;
using FanModeEnum = Clusters::FanControl::FanModeEnum;

CHIP_ERROR FanControlBridgeAttributeAccess::Read(const ConcreteReadAttributePath& path,
                                                 AttributeValueEncoder& encoder)
{
    const auto& r = mStack->Reader();
    switch (path.mAttributeId) {
    case FCAttr::SpeedSetting::Id:
        return encoder.Encode(r.ReadSpeedSetting());
    case FCAttr::FanMode::Id:
        return encoder.Encode(r.ReadFanMode());
    case FCAttr::SpeedCurrent::Id:
        return encoder.Encode(r.ReadSpeedCurrent());
    default:
        return CHIP_NO_ERROR; // fallback to cluster server
    }
}

CHIP_ERROR FanControlBridgeAttributeAccess::Write(const ConcreteDataAttributePath& path,
                                                  AttributeValueDecoder& decoder)
{
    switch (path.mAttributeId) {
    case FCAttr::SpeedSetting::Id: {
        DataModel::Nullable<uint8_t> value;
        ReturnErrorOnFailure(decoder.Decode(value));
        sync::FanSpeed twin;
        if (!value.IsNull()) {
            const uint8_t raw = value.Value();
            // Reject values outside the cluster's SpeedMax range (1..6
            // for our ZAP). The cluster server would normally do this
            // via the SpeedMax constraint, but with External storage we
            // own the validation.
            if (raw < 1 || raw > 6) return CHIP_IM_GLOBAL_STATUS(ConstraintError);
            twin = static_cast<sync::FanLevel>(raw);
        }
        mStack->ApplyIntent(sync::SetSpeedSettingIntent{twin});
        return CHIP_NO_ERROR;
    }
    case FCAttr::FanMode::Id: {
        FanModeEnum mode;
        ReturnErrorOnFailure(decoder.Decode(mode));
        // The cluster spec defines FanMode∈{Off,Low,…,Auto,Smart}. We
        // collapse these to the speed-setting twin:
        sync::FanSpeed twin;
        switch (mode) {
        case FanModeEnum::kOff:    twin = std::nullopt;            break;
        case FanModeEnum::kLow:    twin = sync::FanLevel::Low;     break;
        case FanModeEnum::kMedium: twin = sync::FanLevel::Medium;  break;
        case FanModeEnum::kHigh:   twin = sync::FanLevel::High;    break;
        case FanModeEnum::kOn:     twin = sync::FanLevel::MidLow;  break;
        case FanModeEnum::kAuto:
        case FanModeEnum::kSmart:  twin = std::nullopt;            break;
        default:                   return CHIP_NO_ERROR;            // ignore weird values
        }
        mStack->ApplyIntent(sync::SetSpeedSettingIntent{twin});
        return CHIP_NO_ERROR;
    }
    case FCAttr::SpeedCurrent::Id:
        // Device-reported telemetry; controllers shouldn't write it. The
        // cluster server would normally reject the write; let it. Return
        // success-without-decoding so the server can do its thing.
        return CHIP_NO_ERROR;
    default:
        return CHIP_NO_ERROR;
    }
}

} // namespace sync_aai
