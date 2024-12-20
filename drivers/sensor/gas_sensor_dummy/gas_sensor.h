/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

struct gas_sensor_dummy_data {
	float temperature;
	float pressure;
	float humidity;
	int iaq;
	int co2;
	int voc;
};
