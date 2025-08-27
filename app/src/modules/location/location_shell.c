/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include "location.h"

LOG_MODULE_DECLARE(location_module, CONFIG_APP_LOCATION_LOG_LEVEL);

static int cmd_search(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int err;
	const struct location_msg msg = {
		.type = LOCATION_SEARCH_TRIGGER,
	};

	err = zbus_chan_pub(&LOCATION_CHAN, &msg, K_SECONDS(1));
	if (err) {
		(void)shell_print(sh, "zbus_chan_pub, error: %d", err);
		return 1;
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_cmds,
			       SHELL_CMD(search,
					 NULL,
					 "Trigger location search",
					 cmd_search),
			       SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(att_location, &sub_cmds, "Asset Tracker Template Location CMDs", NULL);
