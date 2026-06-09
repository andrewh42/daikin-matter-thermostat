/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * ChangedAttributesListener — observer interface for SyncCoordinator's dirty-
 * attribute stream. Registered via SyncCoordinator::AddChangedAttributesListener;
 * called outside SyncCoordinator's mutex after every mutation that produces a
 * non-empty dirty-attribute set.
 *
 * Listeners receive a vector of `sync::LogicalAttribute` values naming what
 * changed in the bridge's view. Translation to Matter cluster/attribute
 * coordinates happens at the listener (see sync_aai::toMatterAddress).
 *
 * Listeners must not call back into mutating SyncCoordinator methods
 * synchronously — schedule onto the appropriate thread first if a
 * mutation is needed in response.
 */
#pragma once

#include "logical_attribute.h"

#include <vector>

namespace sync {

class ChangedAttributesListener {
public:
    virtual ~ChangedAttributesListener() = default;

    /// Called outside SyncCoordinator's mutex. `attributes` is non-empty.
    virtual void OnChangedAttributes(const std::vector<LogicalAttribute>& attributes) = 0;
};

} // namespace sync
