/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <time.h>

#include "ntn.h"

LOG_MODULE_DECLARE(ntn, CONFIG_APP_NTN_LOG_LEVEL);

static int cmd_set_time(const struct shell *sh, size_t argc, char **argv)
{
	int err;
	struct ntn_msg msg = {
		.type = NTN_SHELL_SET_TIME,
	};

	if (argc != 2) {
		shell_print(sh, "Usage: att_ntn_set_time <YYYY-MM-DD-HH:MM:SS>");
		shell_print(sh, "Example: att_ntn_set_time \"2025-10-07-14:30:00\"");
		return 1;
	}

	/* Validate time format */
	struct tm test_time = {0};
	if (strptime(argv[1], "%Y-%m-%d-%H:%M:%S", &test_time) == NULL) {
		shell_print(sh, "Invalid time format. Use: YYYY-MM-DD-HH:MM:SS");
		return 1;
	}

	strncpy(msg.time_of_pass, argv[1], sizeof(msg.time_of_pass) - 1);
	msg.time_of_pass[sizeof(msg.time_of_pass) - 1] = '\0';

	err = zbus_chan_pub(&NTN_CHAN, &msg, K_SECONDS(1));
	if (err) {
		shell_print(sh, "Failed to publish message, error: %d", err);
		return 1;
	}

	shell_print(sh, "Setting new time of pass to: %s", msg.time_of_pass);
	return 0;
}

SHELL_CMD_REGISTER(att_ntn_set_time, NULL,
		  "Set new time of pass for NTN module (format: YYYY-MM-DD-HH:MM:SS)",
		  cmd_set_time);
