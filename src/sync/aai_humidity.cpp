/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#include "aai_humidity.h"

#include "sync_reader.h"
#include "sync_stack.h"

#include <app/AttributeAccessInterface.h>

namespace sync_aai {

using namespace chip;
using namespace chip::app;
namespace RHAttr = Clusters::RelativeHumidityMeasurement::Attributes;

CHIP_ERROR HumidityBridgeAttributeAccess::Read(const ConcreteReadAttributePath& path,
                                               AttributeValueEncoder& encoder)
{
    if (path.mAttributeId == RHAttr::MeasuredValue::Id) {
        return encoder.Encode(mStack->Reader().ReadHumidityCentiPercent());
    }
    // Return success-without-encoding so the cluster server's fallback
    // (RAM/ember) handles any attribute we don't externalise.
    return CHIP_NO_ERROR;
}

} // namespace sync_aai
