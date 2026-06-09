/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#include "aai_installer.h"

#include "sync_coordinator.h"

#include <app/AttributeAccessInterfaceRegistry.h>
#include <app/clusters/thermostat-server/thermostat-server.h>

namespace sync {

CHIP_ERROR AAIInstaller::Install(chip::EndpointId endpoint, SyncCoordinator* stack)
{
    using namespace chip::app;

    if (mInstalled) return CHIP_NO_ERROR;

    mOnOff.Bind(stack);
    mThermostat.Bind(stack);
    mFanControl.Bind(stack);
    mHumidity.Bind(stack);

    // Replace the SDK's wildcard ThermostatAttrAccess. It's a file-local
    // static inside thermostat-server.cpp (line 80), so we can't reach it
    // by name from here — but Get() finds it by (endpoint, cluster) match
    // because the registration uses Optional<EndpointId>::Missing().
    auto* existing = AttributeAccessInterfaceRegistry::Instance().Get(
        endpoint, Clusters::Thermostat::Id);
    if (existing != nullptr && existing != &mThermostat) {
        AttributeAccessInterfaceRegistry::Instance().Unregister(existing);
    }

    bool ok = true;
    ok = AttributeAccessInterfaceRegistry::Instance().Register(&mThermostat) && ok;
    ok = AttributeAccessInterfaceRegistry::Instance().Register(&mOnOff)      && ok;
    ok = AttributeAccessInterfaceRegistry::Instance().Register(&mFanControl) && ok;
    ok = AttributeAccessInterfaceRegistry::Instance().Register(&mHumidity)   && ok;

    if (!ok) {
        // Best-effort rollback: unregister anything we managed to register.
        Uninstall();
        return CHIP_ERROR_INTERNAL;
    }

    mInstalled = true;
    return CHIP_NO_ERROR;
}

void AAIInstaller::Uninstall()
{
    using namespace chip::app;
    AttributeAccessInterfaceRegistry::Instance().Unregister(&mThermostat);
    AttributeAccessInterfaceRegistry::Instance().Unregister(&mOnOff);
    AttributeAccessInterfaceRegistry::Instance().Unregister(&mFanControl);
    AttributeAccessInterfaceRegistry::Instance().Unregister(&mHumidity);
    mInstalled = false;
}

} // namespace sync
