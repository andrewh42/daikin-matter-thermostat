/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <app-common/zap-generated/ids/Clusters.h>
#include <app/ConcreteAttributePath.h>
#include <lib/support/logging/CHIPLogging.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace chip;
using namespace chip::app::Clusters;

void MatterPostAttributeChangeCallback(const chip::app::ConcreteAttributePath& attributePath, uint8_t type,
                                       uint16_t size, uint8_t* value)
{
    // Thermostat / OnOff / FanControl writes are intercepted by AAI in
    // sync/ before reaching the cluster server, so this callback fires
    // only for clusters whose attributes still flow through the cluster
    // server's storage. Identify is the lone case in this codebase.
    if (attributePath.mClusterId == Identify::Id) {
        ChipLogProgress(Zcl, "Identify attribute ID: " ChipLogFormatMEI " Type: %u Value: %u, length %u",
                        ChipLogValueMEI(attributePath.mAttributeId), type, *value, size);
    }
}

void emberAfOnOffClusterInitCallback(EndpointId endpoint)
{
    ChipLogProgress(Zcl, "OnOff cluster init callback for endpoint %u", endpoint);
}
