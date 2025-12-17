/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include "network.h"

LOG_MODULE_DECLARE(network, CONFIG_APP_NETWORK_LOG_LEVEL);

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

SHELL_STATIC_SUBCMD_SET_CREATE(sub_cmds,
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

SHELL_CMD_REGISTER(att_network,
		   &sub_cmds,
		   "Asset Tracker Template Network module commands",
		   NULL);
