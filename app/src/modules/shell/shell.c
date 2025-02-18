/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include "message_channel.h"

LOG_MODULE_REGISTER(shell, CONFIG_APP_SHELL_LOG_LEVEL);

static int cmd_publish_on_payload_chan(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 3) {
		shell_print(sh, "Invalid number of arguments (%d)", argc);
		shell_print(sh, "Usage: zbus publish payload_chan <channel> <payload>");
		return -EINVAL;
	}

	const char *channel_name = argv[1];
	const char *payload = argv[2];
	size_t payload_size = strlen(payload);

	if (payload_size == 0 || payload_size > CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_POOL_SIZE) {
		shell_print(sh, "Invalid payload size: %zu", payload_size);
		return -EINVAL;
	}

	if (strcmp(channel_name, "payload_chan") != 0) {
		shell_print(sh, "Invalid channel name: %s", channel_name);
		return -EINVAL;
	}

	// Assuming the payload is a string message
	int err = zbus_chan_pub(&PAYLOAD_CHAN, payload, K_SECONDS(1));
	if (err) {
		shell_print(sh, "Failed to publish on channel %s, error: %d", channel_name, err);
		return err;
	}

	shell_print(sh, "Message published on channel %s: %s", channel_name, payload);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_zbus_publish,
	SHELL_CMD(payload_chan, NULL, "Publish on payload channel", cmd_publish_on_payload_chan),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(zbus, &sub_zbus, "Zbus shell", NULL);
