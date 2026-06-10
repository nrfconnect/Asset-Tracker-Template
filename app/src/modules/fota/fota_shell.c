/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/shell/shell.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "fota.h"

static int cmd_poll(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int err;
	const struct fota_msg msg = {
		.type = FOTA_POLL_REQUEST,
	};

	err = zbus_chan_pub(&fota_chan, &msg, PUB_TIMEOUT);
	if (err) {
		(void)shell_print(sh, "zbus_chan_pub, error: %d", err);
		return 1;
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_cmds,
	SHELL_CMD(poll,
		  NULL,
		  "Poll nRF Cloud for pending firmware updates",
		  cmd_poll),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(att_fota,
		   &sub_cmds,
		   "Asset Tracker Template FOTA module commands",
		   NULL);
