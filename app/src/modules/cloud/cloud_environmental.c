/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <net/nrf_cloud_coap.h>

#include "cloud_environmental.h"

LOG_MODULE_DECLARE(cloud, CONFIG_APP_CLOUD_LOG_LEVEL);

int cloud_environmental_send(const struct environmental_msg *env,
			     int64_t timestamp_ms,
			     bool confirmable)
{
	int err;

	err = nrf_cloud_coap_sensor_send(NRF_CLOUD_JSON_APPID_VAL_TEMP,
					 env->temperature,
					 timestamp_ms,
					 confirmable);
	if (err) {
		LOG_ERR("Failed to send temperature data to cloud, error: %d", err);
		return err;
	}

	err = nrf_cloud_coap_sensor_send(NRF_CLOUD_JSON_APPID_VAL_AIR_PRESS,
					 env->pressure,
					 timestamp_ms,
					 confirmable);
	if (err) {
		LOG_ERR("Failed to send pressure data to cloud, error: %d", err);
		return err;
	}

	err = nrf_cloud_coap_sensor_send(NRF_CLOUD_JSON_APPID_VAL_HUMID,
					 env->humidity,
					 timestamp_ms,
					 confirmable);
	if (err) {
		LOG_ERR("Failed to send humidity data to cloud, error: %d", err);
		return err;
	}

	LOG_DBG("Environmental data sent to cloud: T=%.1f°C, P=%.1fhPa, H=%.1f%%",
		(double)env->temperature, (double)env->pressure, (double)env->humidity);

	return 0;
}
