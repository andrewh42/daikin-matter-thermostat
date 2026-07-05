/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#pragma once

#include <app/CommandHandlerInterface.h>
#include <app-common/zap-generated/cluster-objects.h>

namespace sync { class SyncCoordinator; }

namespace sync_aai {

/**
 * OnOffCommandHandler intercepts the OnOff cluster's On/Off/Toggle commands
 * and routes them to the bridge as `SetOnOffIntent`s.
 *
 * It exists because the OnOff attribute is externalised to the bridge AAI,
 * but the SDK's `OnOffServer` command handler reads/writes that attribute
 * through the ember accessor path (`Attributes::OnOff::Get/Set`), which
 * bypasses the AAI and hits the unimplemented `emberAfExternalAttribute*`
 * callbacks — so every On/Off/Toggle would fail with `ERR: reading on/off`.
 *
 * CHIP's command dispatch consults the `CommandHandlerInterfaceRegistry`
 * before the ember cluster server, so registering this handler makes the
 * SDK's failing `OnOffServer` path unreachable while keeping `BridgeKernel`
 * the sole owner of power state (the command becomes an intent, exactly like
 * an attribute write does through the AAI).
 */
class OnOffCommandHandler : public chip::app::CommandHandlerInterface {
public:
    OnOffCommandHandler()
        : CommandHandlerInterface(chip::Optional<chip::EndpointId>::Missing(),
                                  chip::app::Clusters::OnOff::Id)
    {
    }

    void Bind(sync::SyncCoordinator* stack) { mStack = stack; }

    void InvokeCommand(HandlerContext& ctx) override;

private:
    sync::SyncCoordinator* mStack{nullptr};
};

/**
 * ThermostatCommandHandler intercepts the Thermostat cluster's
 * `SetpointRaiseLower` command and routes it to the bridge as setpoint
 * intents.
 *
 * Same rationale as OnOffCommandHandler: the occupied setpoints are
 * externalised to the bridge AAI, but the SDK's `SetpointRaiseLower` handler
 * reads/writes them via ember accessors that bypass the AAI and fail against
 * external storage. This handler reads the live setpoints from the
 * coordinator, applies the delta (clamped to the configured limits), and
 * emits `SetOccupiedHeatingSetpointIntent` / `SetOccupiedCoolingSetpointIntent`
 * — the same intents the AAI emits for a direct setpoint write.
 *
 * Note: the dead-band coupling the SDK applies in Auto mode is intentionally
 * not replicated here; mode-aware setpoint routing is the reconciler's job.
 */
class ThermostatCommandHandler : public chip::app::CommandHandlerInterface {
public:
    ThermostatCommandHandler()
        : CommandHandlerInterface(chip::Optional<chip::EndpointId>::Missing(),
                                  chip::app::Clusters::Thermostat::Id)
    {
    }

    void Bind(sync::SyncCoordinator* stack) { mStack = stack; }

    void InvokeCommand(HandlerContext& ctx) override;

private:
    void HandleSetpointRaiseLower(
        HandlerContext& ctx,
        chip::app::Clusters::Thermostat::SetpointRaiseLowerModeEnum mode,
        int8_t amount);

    sync::SyncCoordinator* mStack{nullptr};
};

} // namespace sync_aai
