/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "network.h"

LOG_MODULE_DECLARE(network_ntn, CONFIG_APP_NETWORK_NTN_LOG_LEVEL);

static int cmd_connect(const struct shell *sh,
		       size_t argc, char **argv)
{
	int err;
	const struct network_msg msg = {
		.type = NETWORK_CONNECT,
	};

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
	if (err) {
		shell_print(sh, "zbus_chan_pub: %d", err);
		return 1;
	}

	return 0;
}

static int cmd_disconnect(const struct shell *sh,
			  size_t argc, char **argv)
{
	int err;
	const struct network_msg msg = {
		.type = NETWORK_DISCONNECT,
	};

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
	if (err) {
		shell_print(sh, "zbus_chan_pub: %d", err);
		return 1;
	}

	return 0;
}

static int cmd_search_stop(const struct shell *sh,
			   size_t argc, char **argv)
{
	int err;
	const struct network_msg msg = {
		.type = NETWORK_SEARCH_STOP,
	};

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
	if (err) {
		shell_print(sh, "zbus_chan_pub: %d", err);
		return 1;
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_cmds,
	SHELL_CMD(connect, NULL,
		  "Connect (TN first, then NTN fallback)",
		  cmd_connect),
	SHELL_CMD(disconnect, NULL,
		  "Disconnect from network",
		  cmd_disconnect),
	SHELL_CMD(search_stop, NULL,
		  "Stop ongoing network search",
		  cmd_search_stop),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(att_network,
		   &sub_cmds,
		   "NTN network module commands",
		   NULL);
