/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * SensorField<T>
 * --------------
 * One-way observation-only counterpart to TwinField. The bridge has fields
 * that flow in only one direction — device-reported sensor readings that no
 * Matter controller can write to (indoor/outdoor temperature, humidity,
 * refrigerant-valve state, reachable). For those, the TwinField machinery
 * (desired / inFlight / attribution / dirty) is dead weight, and worse,
 * surfaces methods that no caller should ever invoke.
 *
 * SensorField encodes the asymmetry in the type system: it exposes only
 * what observation-only fields need — applyObservation() and observed().
 *
 * Header-only and free of CHIP/Zephyr dependencies, same as TwinField.
 */
#pragma once

#include <utility>

namespace sync {

template <typename T>
class SensorField {
public:
    explicit SensorField(T initial = T{}) : mValue(std::move(initial)) {}

    const T& observed() const { return mValue; }

    void applyObservation(T value) { mValue = std::move(value); }

private:
    T mValue;
};

} // namespace sync
