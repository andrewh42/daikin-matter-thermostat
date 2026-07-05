/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#pragma once

#include <app/AttributeAccessInterface.h>
#include <app-common/zap-generated/cluster-objects.h>

namespace sync { class SyncCoordinator; }

namespace sync_aai {

/**
 * FanControlBridgeAttributeAccess is the AAI for FanControl's
 * SpeedSetting / FanMode / SpeedCurrent / PercentSetting / PercentCurrent.
 * The cluster server registers no AAI of its own, so plain insertion works.
 *
 * Spec-compliant mapping (Matter 1.5 §4.4; SpeedMax=6, FanModeSequence
 * OffLowMedHighAuto). The S21 fan is level-native (1..6) plus Auto; "Off"
 * has no fan representation, so the 0/Off range maps to the OnOff power axis:
 *   - SpeedSetting / PercentSetting null write → no-op (SHALL NOT change).
 *   - SpeedSetting=0 / PercentSetting=0 / FanMode=Off → power the AC off.
 *   - SpeedSetting 1..6 → set the fan level; >6 → CONSTRAINT_ERROR.
 *   - PercentSetting 1..100 → set the fan (exact percent remembered); >100 →
 *       CONSTRAINT_ERROR.
 *   - FanMode Low/Medium/High → representative level (2/4/6); On → High;
 *       Auto/Smart → Auto (null); unsupported → CONSTRAINT_ERROR.
 *   - A non-off fan write while the AC is off powers it on (handled in the
 *       reconciler), restoring SystemMode to the retained mode.
 *   - SpeedCurrent / PercentCurrent → read-only telemetry; writes ignored.
 */
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
