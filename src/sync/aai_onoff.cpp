/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#include "aai_onoff.h"

#include "sync_coordinator.h"

#include <app/AttributeAccessInterface.h>
#include <protocols/interaction_model/StatusCode.h>

namespace sync_aai {

using namespace chip;
using namespace chip::app;
namespace OOAttr = Clusters::OnOff::Attributes;

CHIP_ERROR OnOffBridgeAttributeAccess::Read(const ConcreteReadAttributePath& path,
                                            AttributeValueEncoder& encoder)
{
    if (path.mAttributeId == OOAttr::OnOff::Id) {
        return encoder.Encode(mStack->ReadOnOff());
    }
    // Return success-without-encoding so the cluster server's fallback
    // (RAM/ember) handles any attribute we don't externalise.
    return CHIP_NO_ERROR;
}

CHIP_ERROR OnOffBridgeAttributeAccess::Write(const ConcreteDataAttributePath& path,
                                             AttributeValueDecoder& decoder)
{
    if (path.mAttributeId == OOAttr::OnOff::Id) {
        bool value;
        ReturnErrorOnFailure(decoder.Decode(value));
        mStack->ApplyIntent(sync::SetOnOffIntent{value});
        return CHIP_NO_ERROR;
    }
    return CHIP_NO_ERROR; // fall through to cluster server
}

} // namespace sync_aai
