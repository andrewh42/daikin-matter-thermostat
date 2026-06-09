/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * FanControlBridgeAttributeAccess
 * -------------------------------
 * AAI for FanControl's SpeedSetting / FanMode / SpeedCurrent. The
 * cluster server registers no AAI of its own, so plain insertion works.
 *
 * Mapping policy:
 *   - Writes to FanMode (kAuto/kSmart) → speed setting nullopt (Auto)
 *   - Writes to FanMode (kOn) → preserve current speed setting (or set to 3)
 *   - Writes to FanMode (kOff) → bridge translates to OnOff=false via a
 *       separate intent; we ignore here since the OnOff cluster owns that.
 *   - Writes to SpeedSetting (non-null) → speed setting = decoded value
 *   - Writes to SpeedSetting null → speed setting nullopt (Auto)
 *   - Writes to SpeedCurrent → ignored (read-only-ish device telemetry)
 */
#pragma once

#include <app/AttributeAccessInterface.h>
#include <app-common/zap-generated/cluster-objects.h>

namespace sync { class SyncCoordinator; }

namespace sync_aai {

class FanControlBridgeAttributeAccess : public chip::app::AttributeAccessInterface {
public:
    FanControlBridgeAttributeAccess()
        : AttributeAccessInterface(chip::Optional<chip::EndpointId>::Missing(),
                                   chip::app::Clusters::FanControl::Id)
    {
    }

    void Bind(sync::SyncCoordinator* stack) { mStack = stack; }

    CHIP_ERROR Read(const chip::app::ConcreteReadAttributePath& path,
                    chip::app::AttributeValueEncoder& encoder) override;

    CHIP_ERROR Write(const chip::app::ConcreteDataAttributePath& path,
                     chip::app::AttributeValueDecoder& decoder) override;

private:
    sync::SyncCoordinator* mStack{nullptr};
};

} // namespace sync_aai
