/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * ChangedAttributesListener — observer interface for SyncStack's dirty-path
 * stream. Registered via SyncStack::AddChangedAttributesListener; called
 * outside SyncStack's mutex after every mutation that produces a non-empty
 * dirty-attribute set.
 *
 * Listeners must not call back into mutating SyncStack methods
 * synchronously — schedule onto the appropriate thread first if a
 * mutation is needed in response.
 */
#pragma once

#include "matter_attribute_path.h"

#include <vector>

namespace sync {

class ChangedAttributesListener {
public:
    virtual ~ChangedAttributesListener() = default;

    /// Called outside SyncStack's mutex. `paths` is non-empty.
    virtual void OnChangedAttributes(const std::vector<MatterAttributePath>& paths) = 0;
};

} // namespace sync
