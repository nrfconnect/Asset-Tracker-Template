/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>

#include "ntn.h"

LOG_MODULE_DECLARE(ntn_module, CONFIG_APP_NTN_LOG_LEVEL);

static int parse_double(const char *str, double *value)
{
	char *end;

	*value = strtod(str, &end);
	if ((end == str) || (*end != '\0')) {
		return -EINVAL;
	}

	return 0;
}

static int cmd_ntn_location_set(const struct shell *sh, size_t argc, char **argv)
{
	int err;
	double lat, lon, alt;
	double accuracy = CONFIG_APP_NTN_SHELL_DEFAULT_ACCURACY;
	/* Static to keep the large PVT frame off the shell thread stack */
	static struct ntn_msg msg;

	if (parse_double(argv[1], &lat) || (lat < -90.0) || (lat > 90.0)) {
		(void)shell_print(sh, "Invalid latitude: %s", argv[1]);
		return 1;
	}

	if (parse_double(argv[2], &lon) || (lon < -180.0) || (lon > 180.0)) {
		(void)shell_print(sh, "Invalid longitude: %s", argv[2]);
		return 1;
	}

	if (parse_double(argv[3], &alt)) {
		(void)shell_print(sh, "Invalid altitude: %s", argv[3]);
		return 1;
	}

	if ((argc == 5) && (parse_double(argv[4], &accuracy) || (accuracy < 0.0))) {
		(void)shell_print(sh, "Invalid accuracy: %s", argv[4]);
		return 1;
	}

	memset(&msg, 0, sizeof(msg));
	msg.type = NTN_LOCATION_SET;
	msg.pvt.latitude = lat;
	msg.pvt.longitude = lon;
	msg.pvt.altitude = (float)alt;
	msg.pvt.accuracy = (float)accuracy;

	err = zbus_chan_pub(&NTN_CHAN, &msg, K_SECONDS(1));
	if (err) {
		(void)shell_print(sh, "zbus_chan_pub, error: %d", err);
		return 1;
	}

	return 0;
}

static int cmd_ntn_location_clear(const struct shell *sh, size_t argc, char **argv)
{
	int err;
	struct ntn_msg msg = {
		.type = NTN_LOCATION_CLEAR
	};

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	err = zbus_chan_pub(&NTN_CHAN, &msg, K_SECONDS(1));
	if (err) {
		(void)shell_print(sh, "zbus_chan_pub, error: %d", err);
		return 1;
	}

	return 0;
}

SHELL_CMD_ARG_REGISTER(att_ntn_location_set, NULL,
		       "Set NTN location: <latitude> <longitude> <altitude> [accuracy]",
		       cmd_ntn_location_set, 4, 1);
SHELL_CMD_REGISTER(att_ntn_location_clear, NULL,
		   "Invalidate the stored NTN location", cmd_ntn_location_clear);
