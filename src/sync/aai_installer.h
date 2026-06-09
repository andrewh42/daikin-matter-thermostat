/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#pragma once

#include "aai_fan_control.h"
#include "aai_humidity.h"
#include "aai_onoff.h"
#include "aai_thermostat.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <lib/core/CHIPError.h>

namespace sync {

class SyncCoordinator;

/**
 * AAIInstaller owns the four bridge AAI instances (Thermostat, OnOff,
 * FanControl, Humidity) and handles CHIP AAI registry plumbing on
 * Init/Shutdown.
 *
 * The Thermostat cluster server registers its own wildcard AAI
 * (`gThermostatAttrAccess`) during cluster-server init. The bridge needs
 * the wildcard slot for itself, so Install() locates and unregisters the
 * existing AAI before registering the bridge's own.
 *
 * Lifetime: AAI instances live as members of the installer, which is in
 * turn owned by SyncCoordinator. CHIP's registry holds raw pointers; as
 * long as the installer outlives Shutdown, those pointers stay valid.
 *
 * Not host-testable in isolation: the AAI subclasses inherit from
 * `chip::app::AttributeAccessInterface`, and the registry plumbing is
 * CHIP-flavored end to end. The installer is verified by on-device
 * bring-up (the bridge appears as a Thermostat to controllers); the
 * domain logic that *would* be host-testable already lives in
 * aai_translation.h and is exercised by test_aai_translation.cpp.
 */
class AAIInstaller {
public:
    AAIInstaller() = default;
    AAIInstaller(const AAIInstaller&)            = delete;
    AAIInstaller& operator=(const AAIInstaller&) = delete;

    /// Bind the four AAIs to `stack`, unregister the cluster server's
    /// pre-existing Thermostat AAI, and register the bridge's AAIs against
    /// the CHIP registry. Idempotent. On any per-AAI register failure the
    /// installer rolls back partial state and returns CHIP_ERROR_INTERNAL.
    CHIP_ERROR Install(chip::EndpointId endpoint, SyncCoordinator* stack);

    /// Reverse of Install. Safe before Install / after Uninstall.
    void Uninstall();

    bool Installed() const { return mInstalled; }

private:
    bool                                          mInstalled{false};
    sync_aai::OnOffBridgeAttributeAccess          mOnOff;
    sync_aai::ThermostatBridgeAttributeAccess     mThermostat;
    sync_aai::FanControlBridgeAttributeAccess     mFanControl;
    sync_aai::HumidityBridgeAttributeAccess       mHumidity;
};

} // namespace sync
