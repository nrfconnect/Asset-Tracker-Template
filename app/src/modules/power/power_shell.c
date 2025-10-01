/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/shell/shell.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <errno.h>

#include "power.h"

LOG_MODULE_DECLARE(power, CONFIG_APP_POWER_LOG_LEVEL);

static bool sample_requested;

void power_shell_listener_callback(const struct zbus_channel *chan)
{
	if (!sample_requested) {
		return;
	}

	const struct power_msg *msg = zbus_chan_const_msg(chan);

	if (msg->type == POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE) {
		LOG_INF("Battery state of charge: %.2f%%", msg->percentage);
		LOG_INF("Battery voltage: %.2fV", msg->voltage);
		LOG_INF("Charging: %s", msg->charging ? "Yes" : "No");
		sample_requested = false;
	}
}

ZBUS_LISTENER_DEFINE(power_shell_listener, power_shell_listener_callback);

ZBUS_CHAN_ADD_OBS(POWER_CHAN, power_shell_listener, 0);

static int cmd_power_sample(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int err;
	struct power_msg msg = {
		.type = POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST,
	};

	err = zbus_chan_pub(&POWER_CHAN, &msg, K_NO_WAIT);
	if (err) {
		shell_print(shell, "Failed to send request: %d", err);
		return err;
	}

	sample_requested = true;
	shell_print(shell, "Requesting battery sample...");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_cmds,
	SHELL_CMD(sample, NULL,
		  "Request a battery sample (state of charge, voltage, charging state)",
		  cmd_power_sample),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(att_power, &sub_cmds, "Asset Tracker Template Power CMDs", NULL);
