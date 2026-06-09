/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */
#pragma once

#include "../s21/s21_presentation.h" // OperatingMode, FanMode

#include <cstdint>
#include <optional>

namespace sync {

/**
 * S21OperationalObservation and S21EnvironmentalObservation are the
 * device-side observation types that flow from the S21 layer into the
 * bridge's reconciler. Pair file with write_intent.h on the Matter side:
 * write_intent.h carries controller-side input, this file carries
 * device-side input.
 *
 * Two cadences, two types:
 *
 *   - S21OperationalObservation is built every poll tick from
 *     getOperation() plus (optionally) getUnitState() when the unit is
 *     powered on.
 *
 *   - S21EnvironmentalObservation is built once every
 *     kS21EnvironmentalSensorReadTicks ticks from the three sensor reads
 *     (room temp, outdoor temp, humidity). Producer drops the observation
 *     entirely if any sensor read fails.
 *
 * Each type is complete by contract: every field is a fresh device
 * reading, not "value or stale." `std::optional` inside a type means
 * semantically optional (e.g. refrigerantValveOpen was not queried
 * because the unit is off and RzB2 was skipped), never "we tried and the
 * read failed."
 */
struct S21OperationalObservation {
    bool                onOff;
    OperatingMode       operatingMode;
    int16_t             setpointCelsius;     ///< 0.01 °C, active-mode setpoint
    FanMode             fanMode;
    std::optional<bool> refrigerantValveOpen; ///< nullopt when the unit was
                                              ///< off and RzB2 was skipped.
};

/// See S21OperationalObservation for the shared description of this pair.
struct S21EnvironmentalObservation {
    int16_t indoorTemperatureCelsius;      ///< 0.01 °C
    int16_t outdoorTemperatureCelsius;     ///< 0.01 °C
    uint8_t indoorRelativeHumidityPercent; ///< 0–100 %
};

} // namespace sync
