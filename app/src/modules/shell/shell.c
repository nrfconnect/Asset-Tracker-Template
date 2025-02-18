/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
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
	if (argc < 3) {
		shell_error(sh, "Usage: publish <channel> <payload>");
		return -EINVAL;
	}

	const char *channel_name = argv[1];
	const char *payload = argv[2];
	size_t payload_size = strlen(payload);

	if (payload_size > CONFIG_ZBUS_MAX_PAYLOAD_SIZE) {
		shell_error(sh, "Payload size exceeds maximum allowed size");
		return -EINVAL;
	}

	// Verify the structure of the payload if needed
	// Add your verification logic here

	// Get the reference to the zbus channel using the passed-in name string
	const struct zbus_channel *channel = zbus_chan_get_by_name(channel_name);
	if (!channel) {
		shell_error(sh, "Invalid channel name: %s", channel_name);
		return -EINVAL;
	}

	// Publish the payload on the specified channel
	int err = zbus_chan_pub(channel, payload, K_NO_WAIT);
	if (err) {
		shell_error(sh, "Failed to publish payload on channel: %s", channel_name);
		return err;
	}

	shell_print(sh, "Published payload on channel: %s", channel_name);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_zbus,
	SHELL_CMD(publish, NULL, "Publish on a channel", cmd_publish_on_payload_chan),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(zbus, &sub_zbus, "Zbus shell", NULL);
