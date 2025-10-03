/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/shell/shell.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/kernel.h>
#include <errno.h>

#include "power.h"

#define MAX_MSG_SIZE	    sizeof(struct power_msg)
#define RESPONSE_TIMEOUT_MS 5000

ZBUS_MSG_SUBSCRIBER_DEFINE(power_shell_subscriber);

ZBUS_CHAN_ADD_OBS(POWER_CHAN, power_shell_subscriber, 0);

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

	shell_print(shell, "Requesting battery sample...");

	const struct zbus_channel *chan;
	uint8_t msg_buf[MAX_MSG_SIZE];
	int end_time = k_uptime_get() + RESPONSE_TIMEOUT_MS;

	while (k_uptime_get() < end_time) {
		err = zbus_sub_wait_msg(&power_shell_subscriber, &chan, msg_buf, K_MSEC(100));

		if (err == -EAGAIN || err == -ENOMSG) {
			/* Timeout, continue waiting */
			continue;
		}

		if (err) {
			shell_print(shell, "Error receiving message: %d", err);
			return err;
		}

		struct power_msg resp = MSG_TO_POWER_MSG(msg_buf);

		/* Skip request messages (we might receive our own published message) */
		if (resp.type == POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST) {
			continue;
		}

		if (resp.type == POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE) {
			shell_print(shell, "Battery state of charge: %.2f%%", resp.percentage);
			shell_print(shell, "Battery voltage: %.2fV", resp.voltage);
			shell_print(shell, "Charging: %s", resp.charging ? "Yes" : "No");
			return 0;
		}
	}

	shell_print(shell, "No response received (timeout)");
	return 1;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_cmds,
	SHELL_CMD(sample, NULL,
		  "Request a battery sample (state of charge, voltage, charging state)",
		  cmd_power_sample),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(att_power, &sub_cmds, "Asset Tracker Template Power CMDs", NULL);
