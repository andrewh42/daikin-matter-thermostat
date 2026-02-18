/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app/ConcreteAttributePath.h>

#include <lib/core/CHIPError.h>

using namespace chip;
using namespace chip::app;

class TemperatureManager {
public:
	static TemperatureManager &Instance()
	{
		static TemperatureManager sTemperatureManager;
		return sTemperatureManager;
	};

	CHIP_ERROR Init();
	void AttributeChangeHandler(const ConcreteAttributePath &attributePath, uint8_t *value, uint16_t size);
	DataModel::Nullable<int16_t> GetLocalTemp();
	DataModel::Nullable<int16_t> GetOutdoorTemp();

	void LogThermostatStatus();

private:
	bool mOnOff;
	DataModel::Nullable<int16_t> mLocalTempCelsius;
	DataModel::Nullable<int16_t> mOutdoorTempCelsius;
	int16_t mCoolingCelsiusSetPoint;
	int16_t mHeatingCelsiusSetPoint;
	Clusters::Thermostat::SystemModeEnum mThermMode;

	void OnOffAttributeChangeHandler(AttributeId attributeId, uint8_t *value, uint16_t size);
	void TemperatureAttributeChangeHandler(AttributeId attributeId, uint8_t *value, uint16_t size);
	CHIP_ERROR InitLed();
	const char* GetThermModeStr();
	void UpdatePowerIndicator();
};
