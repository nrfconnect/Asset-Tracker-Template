/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "ntn.h"

LOG_MODULE_DECLARE(ntn_module, CONFIG_APP_NTN_LOG_LEVEL);

static int ntn_msg_send(const struct shell *sh, const struct ntn_msg *msg)
{
	int err;

	err = zbus_chan_pub(&ntn_chan, msg, PUB_TIMEOUT);
	if (err) {
		(void)shell_print(sh, "zbus_chan_pub, error: %d", err);
		return 1;
	}

	return 0;
}

static int cmd_start(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	const struct ntn_msg msg = {
		.type = NTN_FALLBACK_START_REQUEST,
	};

	return ntn_msg_send(sh, &msg);
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	const struct ntn_msg msg = {
		.type = NTN_FALLBACK_STOP_REQUEST,
	};

	return ntn_msg_send(sh, &msg);
}

static int cmd_send(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	const struct ntn_msg msg = {
		.type = NTN_UPLINK_REQUEST,
	};

	return ntn_msg_send(sh, &msg);
}

static int cmd_location(const struct shell *sh, size_t argc, char **argv)
{
	struct ntn_msg msg = {
		.type = NTN_LOCATION_SET,
		.location = {
			.latitude = strtod(argv[1], NULL),
			.longitude = strtod(argv[2], NULL),
			.altitude = strtof(argv[3], NULL),
			.accuracy = (argc > 4) ? strtof(argv[4], NULL) : 10.0f,
		},
	};

	if ((msg.location.latitude < -90.0) || (msg.location.latitude > 90.0) ||
	    (msg.location.longitude < -180.0) || (msg.location.longitude > 180.0)) {
		(void)shell_print(sh, "Invalid coordinates");
		return 1;
	}

	return ntn_msg_send(sh, &msg);
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_cmds,
			       SHELL_CMD(start,
					 NULL,
					 "Fall back to NTN now",
					 cmd_start),
			       SHELL_CMD(stop,
					 NULL,
					 "Stop using NTN and search for a terrestrial network",
					 cmd_stop),
			       SHELL_CMD(send,
					 NULL,
					 "Send the last known location over NTN now",
					 cmd_send),
			       SHELL_CMD_ARG(location,
					     NULL,
					     "Set location: <lat> <lon> <alt_m> [accuracy_m]",
					     cmd_location, 4, 1),
			       SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(att_ntn,
		   &sub_cmds,
		   "Asset Tracker Template NTN module commands",
		   NULL);
