/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#include "aai_fan_control.h"

#include "aai_translation.h"
#include "fan_mapping.h"
#include "sync_coordinator.h"
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
    auto& r = *mStack;
    switch (path.mAttributeId) {
    case FCAttr::SpeedSetting::Id:
        // nullopt ⇔ Auto (null); 0 ⇔ Off (unit powered off); else level.
        return encoder.Encode(sync_aai::wrap(r.ReadSpeedSetting()));
    case FCAttr::FanMode::Id:
        return encoder.Encode(sync_aai::toMatterFanMode(r.ReadFanMode()));
    case FCAttr::SpeedCurrent::Id:
        return encoder.Encode(r.ReadSpeedCurrent());
    case FCAttr::PercentSetting::Id:
        return encoder.Encode(sync_aai::wrap(r.ReadPercentSetting()));
    case FCAttr::PercentCurrent::Id:
        return encoder.Encode(r.ReadPercentCurrent());
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
        if (value.IsNull()) return CHIP_NO_ERROR; // null write: SHALL NOT change (§4.4.6.6)
        const uint8_t raw = value.Value();
        if (raw == 0) {
            // 0/Off: the indoor fan can't stop independently of unit power,
            // so FanControl "Off" maps to powering the AC off.
            mStack->ApplyIntent(sync::SetOnOffIntent{false});
            return CHIP_NO_ERROR;
        }
        if (raw > sync::kFanSpeedMax) return CHIP_IM_GLOBAL_STATUS(ConstraintError);
        mStack->ApplyIntent(
            sync::SetSpeedSettingIntent{sync::FanSpeed{static_cast<sync::FanLevel>(raw)}});
        return CHIP_NO_ERROR;
    }
    case FCAttr::PercentSetting::Id: {
        DataModel::Nullable<uint8_t> value;
        ReturnErrorOnFailure(decoder.Decode(value));
        if (value.IsNull()) return CHIP_NO_ERROR; // null write: SHALL NOT change (§4.4.6.3)
        const uint8_t pct = value.Value();
        if (pct == 0) {
            mStack->ApplyIntent(sync::SetOnOffIntent{false}); // 0/Off → power off
            return CHIP_NO_ERROR;
        }
        if (pct > 100) return CHIP_IM_GLOBAL_STATUS(ConstraintError);
        mStack->ApplyIntent(sync::SetPercentSettingIntent{pct});
        return CHIP_NO_ERROR;
    }
    case FCAttr::FanMode::Id: {
        FanModeEnum mode;
        ReturnErrorOnFailure(decoder.Decode(mode));
        const auto w = sync_aai::fromMatterFanMode(mode);
        switch (w.kind) {
        case sync_aai::FanModeWriteKind::PowerOff:
            mStack->ApplyIntent(sync::SetOnOffIntent{false});
            return CHIP_NO_ERROR;
        case sync_aai::FanModeWriteKind::SetSpeed:
            mStack->ApplyIntent(sync::SetSpeedSettingIntent{w.speed});
            return CHIP_NO_ERROR;
        case sync_aai::FanModeWriteKind::Reject:
            return CHIP_IM_GLOBAL_STATUS(ConstraintError);
        }
        return CHIP_NO_ERROR;
    }
    case FCAttr::SpeedCurrent::Id:
    case FCAttr::PercentCurrent::Id:
        // Device-reported telemetry; controllers shouldn't write it. Let the
        // cluster server reject it — return success-without-decoding.
        return CHIP_NO_ERROR;
    default:
        return CHIP_NO_ERROR;
    }
}

} // namespace sync_aai
