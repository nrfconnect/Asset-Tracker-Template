/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <stdio.h>

#include "cloud_mqtt_environmental.h"
#include "cloud.h"

LOG_MODULE_DECLARE(cloud, CONFIG_APP_CLOUD_MQTT_LOG_LEVEL);

#define JSON_BUFFER_SIZE 256

int cloud_mqtt_environmental_send(const struct environmental_msg *env)
{
	int err;
	char json_buf[JSON_BUFFER_SIZE];
	int len;
	struct cloud_msg cloud_msg = {
		.type = CLOUD_PAYLOAD_JSON,
	};

	/* Build JSON payload */
	len = snprintk(json_buf, sizeof(json_buf),
		       "{\"temperature\":\"%.2f\",\"humidity\":\"%.2f\",\"pressure\":\"%.2f\"}",
		       env->temperature,
		       env->humidity,
		       env->pressure);

	if (len < 0 || len >= sizeof(json_buf)) {
		LOG_ERR("Failed to build JSON payload, len: %d", len);
		return -ENOMEM;
	}

	/* Copy to cloud message payload */
	if (len >= sizeof(cloud_msg.payload.buffer)) {
		LOG_ERR("JSON payload too large: %d bytes", len);
		return -ENOMEM;
	}

	memcpy(cloud_msg.payload.buffer, json_buf, len);
	cloud_msg.payload.buffer_data_len = len;

	/* Publish to cloud */
	err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		return err;
	}

	LOG_INF("Environmental data sent to cloud via MQTT");
	LOG_DBG("Payload: %.*s", len, json_buf);
	LOG_DBG("Environmental data sent to cloud: T=%.2f°C, P=%.2fhPa, H=%.2f%%",
		env->temperature, env->pressure, env->humidity);

	return 0;
}
