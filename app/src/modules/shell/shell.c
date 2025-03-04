/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <net/nrf_cloud_defs.h>
#include <date_time.h>

#include "message_channel.h"
#include "button.h"
#include "cloud_module.h"
#include "network.h"

LOG_MODULE_REGISTER(shell, CONFIG_APP_SHELL_LOG_LEVEL);

#define PAYLOAD_MSG_TEMPLATE \
	"{\"" NRF_CLOUD_JSON_MSG_TYPE_KEY "\":\"" NRF_CLOUD_JSON_MSG_TYPE_VAL_DATA "\"," \
	"\"" NRF_CLOUD_JSON_APPID_KEY "\":\"%s\"," \
	"\"" NRF_CLOUD_JSON_DATA_KEY "\":\"%s\"," \
	"\"" NRF_CLOUD_MSG_TIMESTAMP_KEY "\":%lld}"

static int cmd_button_press(const struct shell *sh, size_t argc,
                         char **argv)
{
	int err;
	uint8_t button_number = 1;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	LOG_DBG("Button 1 pressed!");

	err = zbus_chan_pub(&BUTTON_CHAN, &button_number, K_SECONDS(1));
	if (err) {
		(void)shell_print(sh, "zbus_chan_pub, error: %d", err);
		return 1;
	}

	return 0;
}

static int cmd_connect(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int err;
	const struct network_msg msg = {
		.type = NETWORK_CONNECT,
	};

	err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
	if (err) {
		(void)shell_print(sh, "zbus_chan_pub, error: %d", err);
		return 1;
	}

	return 0;
}

static int cmd_disconnect(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int err;
	const struct network_msg msg = {
		.type = NETWORK_DISCONNECT,
	};

	err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
	if (err) {
		(void)shell_print(sh, "zbus_chan_pub, error: %d", err);
		return 1;
	}

	return 0;
}

static int cmd_publish(const struct shell *sh, size_t argc, char **argv)
{
	int err;
	int64_t current_time;
	struct cloud_payload payload = { 0 };

	if (argc != 3) {
		(void)shell_print(sh, "Invalid number of arguments (%d)", argc);
		(void)shell_print(sh, "Usage: zbus publish payload_chan <appid> <data>");
		return 1;
	}

	err = date_time_now(&current_time);
	if (err) {
		(void)shell_print(sh, "Failed to get current time, error: %d", err);
		return 1;
	}

	err = snprintk(payload.buffer, sizeof(payload.buffer),
		       PAYLOAD_MSG_TEMPLATE,
		       argv[1], argv[2], current_time);
	if (err < 0 || err >= sizeof(payload.buffer)) {
		(void)shell_print(sh, "Failed to format payload, error: %d", err);
		return 1;
	}

	payload.buffer_len = err;

	(void)shell_print(sh, "Sending on payload channel: %s (%d bytes)",
			  payload.buffer, payload.buffer_len);

	err = zbus_chan_pub(&PAYLOAD_CHAN, &payload, K_SECONDS(1));
	if (err) {
		(void)shell_print(sh, "zbus_chan_pub, error: %d", err);
		return 1;
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_zbus,
			       SHELL_CMD(button_press,
					 NULL,
					 "Button press",
					 cmd_button_press),
			       SHELL_CMD(publish_payload,
					 NULL,
					 "Publish a payload",
					 cmd_publish),
			       SHELL_CMD(connect,
					 NULL,
					 "Connect to LTE",
					 cmd_connect),
			       SHELL_CMD(disconnect,
					 NULL,
					 "Disconnect from LTE",
					 cmd_disconnect),
			       SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(zbus, &sub_zbus, "Zbus shell", NULL);
