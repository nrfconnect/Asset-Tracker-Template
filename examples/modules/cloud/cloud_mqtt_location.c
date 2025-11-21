/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <stdio.h>
#include <date_time.h>
#include <hw_id.h>

#include "cloud_mqtt_location.h"
#include "cloud.h"

LOG_MODULE_DECLARE(cloud, CONFIG_APP_CLOUD_MQTT_LOG_LEVEL);

#define JSON_BUFFER_SIZE 256

#if defined(CONFIG_LOCATION_METHOD_GNSS)
/* Handle GNSS location data from the location module */
static void handle_gnss_location_data(const struct location_data *location_data)
{
	int err;
	int64_t timestamp_ms;
	char json_buf[JSON_BUFFER_SIZE];
	char hw_id_buf[HW_ID_LEN];
	int len;
	struct cloud_msg cloud_msg = {
		.type = CLOUD_PAYLOAD_JSON,
	};

	LOG_DBG("Handling GNSS location data: lat: %f, lon: %f, acc: %f",
		(double)location_data->latitude,
		(double)location_data->longitude,
		(double)location_data->accuracy);

	/* Get hardware ID (IMEI) */
	err = hw_id_get(hw_id_buf, sizeof(hw_id_buf));
	if (err) {
		LOG_ERR("hw_id_get, error: %d", err);
		return;
	}

	/* Get current timestamp */
	err = date_time_now(&timestamp_ms);
	if (err) {
		LOG_WRN("Failed to get current time, using 0");
		timestamp_ms = 0;
	}

	/* Build JSON payload */
	len = snprintk(json_buf, sizeof(json_buf),
		       "{\"imei\":\"%s\",\"timestamp\":\"%lld\",\"latitude\":\"%.5f\",\"longitude\":\"%.5f\"}",
		       hw_id_buf,
		       timestamp_ms / 1000, /* Convert to seconds */
		       location_data->latitude,
		       location_data->longitude);

	if (len < 0 || len >= sizeof(json_buf)) {
		LOG_ERR("Failed to build JSON payload, len: %d", len);
		return;
	}

	/* Copy to cloud message payload */
	if (len >= sizeof(cloud_msg.payload.buffer)) {
		LOG_ERR("JSON payload too large: %d bytes", len);
		return;
	}

	memcpy(cloud_msg.payload.buffer, json_buf, len);
	cloud_msg.payload.buffer_data_len = len;

	/* Publish to cloud */
	err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		return;
	}

	LOG_INF("GNSS location data sent to cloud via MQTT");
	LOG_DBG("Payload: %.*s", len, json_buf);
}
#endif /* CONFIG_LOCATION_METHOD_GNSS */

void cloud_mqtt_location_handle_message(const struct location_msg *msg)
{
	switch (msg->type) {
#if defined(CONFIG_LOCATION_METHOD_GNSS)
	case LOCATION_GNSS_DATA:
		LOG_DBG("GNSS location data received");
		handle_gnss_location_data(&msg->gnss_data);
		break;
#endif /* CONFIG_LOCATION_METHOD_GNSS */

	default:
		break;
	}
}
