/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <net/nrf_cloud_defs.h>
#include <date_time.h>

#include "cloud.h"

LOG_MODULE_DECLARE(cloud, CONFIG_APP_CLOUD_LOG_LEVEL);

#define PAYLOAD_MSG_TEMPLATE \
	"{\"" NRF_CLOUD_JSON_MSG_TYPE_KEY "\":\"" NRF_CLOUD_JSON_MSG_TYPE_VAL_DATA "\"," \
	"\"" NRF_CLOUD_JSON_APPID_KEY "\":\"%s\"," \
	"\"" NRF_CLOUD_JSON_DATA_KEY "\":\"%s\"," \
	"\"" NRF_CLOUD_MSG_TIMESTAMP_KEY "\":%lld}"

static int cmd_publish(const struct shell *sh, size_t argc, char **argv)
{
	int err;
	int64_t current_time;
	struct cloud_msg msg = {
		.type = CLOUD_PAYLOAD_JSON,
	 };

	if (argc != 3) {
		(void)shell_print(sh, "Invalid number of arguments (%d)", argc);
		(void)shell_print(sh, "Usage: att_cloud_publish <appid> <data>");
		return 1;
	}

	err = date_time_now(&current_time);
	if (err) {
		(void)shell_print(sh, "Failed to get current time, error: %d", err);
		return 1;
	}

	err = snprintk(msg.payload.buffer, sizeof(msg.payload.buffer),
		       PAYLOAD_MSG_TEMPLATE,
		       argv[1], argv[2], current_time);
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

static int cmd_provisioning(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int err;
	struct cloud_msg msg = {
		.type = CLOUD_PROVISIONING_REQUEST,
	};

	err = zbus_chan_pub(&CLOUD_CHAN, &msg, K_SECONDS(1));
	if (err) {
		(void)shell_print(sh, "zbus_chan_pub, error: %d", err);
		return 1;
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(prov_sub_cmds,
			       SHELL_CMD(now,
					 NULL,
					 "Perform provisioning now",
					 cmd_provisioning),
			       SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(att_cloud_publish, NULL, "Asset Tracker Template Cloud CMDs",
		   cmd_publish);
SHELL_CMD_REGISTER(att_cloud_provision, &prov_sub_cmds, "Asset Tracker Template Provisioning CMDs",
		   NULL);
