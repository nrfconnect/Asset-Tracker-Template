/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <date_time.h>

/* Reuse the templates cloud module header */
#include "../../../app/src/modules/cloud/cloud.h"

LOG_MODULE_DECLARE(cloud, CONFIG_APP_CLOUD_MQTT_LOG_LEVEL);

#define PAYLOAD_MSG_TEMPLATE "\"data\":\"%s\",\"ts\":%lld"

static int cmd_publish(const struct shell *sh, size_t argc, char **argv)
{
	int err;
	int64_t current_time;
	struct cloud_msg msg = {
		.type = CLOUD_PAYLOAD_JSON,
	 };

	if (argc != 2) {
		(void)shell_print(sh, "Invalid number of arguments (%d)", argc);
		(void)shell_print(sh, "Usage: att_cloud_publish <data>");
		return 1;
	}

	err = date_time_now(&current_time);
	if (err) {
		(void)shell_print(sh, "Failed to get current time, error: %d", err);
		return 1;
	}

	err = snprintk(msg.payload.buffer, sizeof(msg.payload.buffer),
		       PAYLOAD_MSG_TEMPLATE,
		       argv[1], current_time);
	if (err < 0 || err >= sizeof(msg.payload.buffer)) {
		(void)shell_print(sh, "Failed to format payload, error: %d", err);
		return 1;
	}

	msg.payload.buffer_data_len = err;

	(void)shell_print(sh, "Sending on payload channel: %s (%d bytes)",
			  msg.payload.buffer, msg.payload.buffer_data_len);

	err = zbus_chan_pub(&CLOUD_CHAN, &msg, K_SECONDS(1));
	if (err) {
		(void)shell_print(sh, "zbus_chan_pub, error: %d", err);
		return 1;
	}

	return 0;
}

SHELL_CMD_REGISTER(att_cloud_publish_mqtt, NULL,
		   "Asset Tracker Template Cloud MQTT CMDs", cmd_publish);
