/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>

#include "button.h"

LOG_MODULE_DECLARE(button, CONFIG_APP_BUTTON_LOG_LEVEL);

static int cmd_button_short(const struct shell *sh, size_t argc, char **argv)
{
	int err;
	uint8_t button_number;
	struct button_msg msg = {
		.type = BUTTON_PRESS_SHORT
	};

	if (argc != 2) {
		(void)shell_print(sh, "Invalid number of arguments (%d)", argc);
		(void)shell_print(sh, "Usage: att_button short <button_number>");
		return 1;
	}

	button_number = (uint8_t)strtol(argv[1], NULL, 10);

	if ((button_number != 1) && (button_number != 2)) {
		(void)shell_print(sh, "Invalid button number: %d", button_number);
		return 1;
	}

	msg.button_number = button_number;

	err = zbus_chan_pub(&BUTTON_CHAN, &msg, K_SECONDS(1));
	if (err) {
		(void)shell_print(sh, "zbus_chan_pub, error: %d", err);
		return 1;
	}

	return 0;
}

static int cmd_button_long(const struct shell *sh, size_t argc, char **argv)
{
	int err;
	uint8_t button_number;
	struct button_msg msg = {
		.type = BUTTON_PRESS_LONG
	};

	if (argc != 2) {
		(void)shell_print(sh, "Invalid number of arguments (%d)", argc);
		(void)shell_print(sh, "Usage: att_button long <button_number>");
		return 1;
	}

	button_number = (uint8_t)strtol(argv[1], NULL, 10);

	if ((button_number != 1) && (button_number != 2)) {
		(void)shell_print(sh, "Invalid button number: %d", button_number);
		return 1;
	}

	msg.button_number = button_number;

	err = zbus_chan_pub(&BUTTON_CHAN, &msg, K_SECONDS(1));
	if (err) {
		(void)shell_print(sh, "zbus_chan_pub, error: %d", err);
		return 1;
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_cmds,
			       SHELL_CMD(short,
					 NULL,
					 "Simulate a short button press. Usage: short <button_number>",
					 cmd_button_short),
			       SHELL_CMD(press,
					  NULL,
					  "Simulate a short button press. Usage: press <button_number>",
					  cmd_button_short),
			       SHELL_CMD(long,
					  NULL,
					  "Simulate a long button press. Usage: long <button_number>",
					  cmd_button_long),
			       SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(att_button,
		   &sub_cmds,
		   "Asset Tracker Template Button module commands",
		   NULL);
