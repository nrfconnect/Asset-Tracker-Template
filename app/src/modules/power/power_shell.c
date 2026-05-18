/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/shell/shell.h>
#include <errno.h>

#include "power.h"
#include "app_common.h"

static int cmd_power_sample(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int err;
	struct power_msg msg = {
		.type = POWER_BATTERY_SAMPLE_LOG,
	};
	err = zbus_chan_pub(&power_chan, &msg, PUB_TIMEOUT);
	if (err) {
		shell_error(shell, "Failed to publish battery sample log message, error: %d", err);
		SEND_FATAL_ERROR();
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_cmds,
	SHELL_CMD(sample,
		  NULL,
		  "Get latest sampled battery data (state of charge, voltage, charging state)",
		  cmd_power_sample),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(att_power,
		   &sub_cmds,
		   "Asset Tracker Template Power module commands",
		   NULL);
