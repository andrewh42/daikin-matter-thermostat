/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#include "command_router.h"

#include "setpoint_math.h"
#include "sync_coordinator.h"
#include "write_intent.h"

#include <app/CommandHandler.h>
#include <app/data-model/Decode.h>
#include <app-common/zap-generated/attributes/Accessors.h>
#include <protocols/interaction_model/StatusCode.h>

namespace sync_aai {

using namespace chip;
using namespace chip::app;
using Status = Protocols::InteractionModel::Status;

namespace OnOffCmd = Clusters::OnOff::Commands;
namespace TCmd     = Clusters::Thermostat::Commands;
namespace TAttr    = Clusters::Thermostat::Attributes;
using SetpointMode = Clusters::Thermostat::SetpointRaiseLowerModeEnum;

// ─── OnOff: On / Off / Toggle ───────────────────────────────────────────────

void OnOffCommandHandler::InvokeCommand(HandlerContext& ctx)
{
    bool target;
    switch (ctx.mRequestPath.mCommandId) {
    case OnOffCmd::On::Id:     target = true;                 break;
    case OnOffCmd::Off::Id:    target = false;                break;
    case OnOffCmd::Toggle::Id: target = !mStack->ReadOnOff(); break;
    default:
        // Not one of ours: leave mCommandHandled false so dispatch can fall
        // back. (Only On/Off/Toggle are exposed on this endpoint.)
        return;
    }

    ctx.SetCommandHandled();
    mStack->ApplyIntent(sync::SetOnOffIntent{target});
    ctx.mCommandHandler.AddStatus(ctx.mRequestPath, Status::Success);
}

// ─── Thermostat: SetpointRaiseLower ─────────────────────────────────────────

void ThermostatCommandHandler::InvokeCommand(HandlerContext& ctx)
{
    HandleCommand<TCmd::SetpointRaiseLower::DecodableType>(
        ctx, [this](HandlerContext& innerCtx, const auto& data) {
            HandleSetpointRaiseLower(innerCtx, data.mode, data.amount);
        });
}

void ThermostatCommandHandler::HandleSetpointRaiseLower(HandlerContext& ctx,
                                                        SetpointMode mode, int8_t amount)
{
    const EndpointId ep = ctx.mRequestPath.mEndpointId;
    const bool doHeat   = (mode == SetpointMode::kBoth || mode == SetpointMode::kHeat);
    const bool doCool   = (mode == SetpointMode::kBoth || mode == SetpointMode::kCool);

    if (!doHeat && !doCool) {
        ctx.mCommandHandler.AddStatus(ctx.mRequestPath, Status::InvalidCommand);
        return;
    }

    if (doHeat) {
        // Seed limits with the Abs* defaults; the configured Min/Max limits
        // (RAM-backed, so the ember read works) override when present.
        int16_t lo = 1000, hi = 3000;
        TAttr::MinHeatSetpointLimit::Get(ep, &lo);
        TAttr::MaxHeatSetpointLimit::Get(ep, &hi);
        const int16_t next =
            applySetpointDelta(mStack->ReadOccupiedHeatingSetpoint(), amount, lo, hi);
        mStack->ApplyIntent(sync::SetOccupiedHeatingSetpointIntent{next});
    }

    if (doCool) {
        int16_t lo = 1600, hi = 3200;
        TAttr::MinCoolSetpointLimit::Get(ep, &lo);
        TAttr::MaxCoolSetpointLimit::Get(ep, &hi);
        const int16_t next =
            applySetpointDelta(mStack->ReadOccupiedCoolingSetpoint(), amount, lo, hi);
        mStack->ApplyIntent(sync::SetOccupiedCoolingSetpointIntent{next});
    }

    ctx.mCommandHandler.AddStatus(ctx.mRequestPath, Status::Success);
}

} // namespace sync_aai
