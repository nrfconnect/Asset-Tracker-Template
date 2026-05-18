/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/shell/shell.h>
#include <errno.h>

#include "power.h"

static int cmd_power_sample(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int err;
	struct power_msg msg;

	err = power_get_battery_data(&msg);
	if (err) {
		if (err == -ENODATA) {
			shell_print(shell, "Battery data not available yet");
			return err;
		}

		shell_print(shell, "Failed to get battery data: %d", err);
		return err;
	}

	shell_print(shell, "Battery state of charge: %.2f%%", msg.percentage);
	shell_print(shell, "Battery voltage: %.2fV", msg.voltage);
	shell_print(shell, "Charging: %s", msg.charging ? "Yes" : "No");

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
