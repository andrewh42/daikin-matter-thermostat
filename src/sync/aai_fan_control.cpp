/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#include "aai_fan_control.h"

#include "aai_translation.h"
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
    case FCAttr::SpeedSetting::Id: {
        const auto fs = r.ReadSpeedSetting();
        std::optional<uint8_t> raw =
            fs.has_value() ? std::optional<uint8_t>{static_cast<uint8_t>(*fs)}
                           : std::nullopt;
        return encoder.Encode(sync_aai::wrap(raw));
    }
    case FCAttr::FanMode::Id:
        return encoder.Encode(sync_aai::toMatterFanMode(r.ReadFanIsAuto()));
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
        auto translated = sync_aai::fromMatterFanMode(mode);
        if (!translated.has_value()) return CHIP_NO_ERROR; // ignore weird values
        mStack->ApplyIntent(sync::SetSpeedSettingIntent{*translated});
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
