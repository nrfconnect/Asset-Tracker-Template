/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#define _XOPEN_SOURCE

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include "ntn.h"

LOG_MODULE_DECLARE(ntn_module, CONFIG_APP_NTN_LOG_LEVEL);

static int cmd_ntn_trigger(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct ntn_msg msg = {
		.type = NTN_TRIGGER,
	};
	int err = zbus_chan_pub(&NTN_CHAN, &msg, K_SECONDS(1));

	if (err) {
		shell_print(sh, "Failed to publish NTN trigger message, error: %d", err);
		return 1;
	}

	shell_print(sh, "Triggering NTN state manually");
	return 0;
}

static int cmd_gnss_trigger(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct ntn_msg msg = {
		.type = GNSS_TRIGGER,
	};
	int err = zbus_chan_pub(&NTN_CHAN, &msg, K_SECONDS(1));

	if (err) {
		shell_print(sh, "Failed to publish GNSS message, error: %d", err);
		return 1;
	}

	shell_print(sh, "Triggering GNSS manually");
	return 0;
}

static int cmd_idle_trigger(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct ntn_msg msg = {
		.type = IDLE_TRIGGER,
	};
	int err = zbus_chan_pub(&NTN_CHAN, &msg, K_SECONDS(1));

	if (err) {
		shell_print(sh, "Failed to publish IDLE message, error: %d", err);
		return 1;
	}

	shell_print(sh, "Triggering IDLE manually");
	return 0;
}

static int cmd_set_gnss_location_manual(const struct shell *sh, size_t argc, char **argv)
{
	int err;
	char *endptr;
	struct ntn_msg msg = {
		.type = NTN_SHELL_SET_GNSS_LOCATION,
	};

	if (argc != 5) {
		shell_print(sh, "Usage: att_ntn set_gnss_location <lat> <lon> <alt_m> <validity_s>");
		shell_print(sh, "Example: att_ntn set_gnss_location 63.43 10.39 40.0 1800");
		return 1;
	}

	errno = 0;
	msg.pvt.latitude = strtod(argv[1], &endptr);
	if (errno != 0 || *argv[1] == '\0' || *endptr != '\0' ||
	    msg.pvt.latitude < -90.0 || msg.pvt.latitude > 90.0) {
		shell_print(sh, "Invalid latitude. Expected a value in range [-90, 90]");
		return 1;
	}

	errno = 0;
	msg.pvt.longitude = strtod(argv[2], &endptr);
	if (errno != 0 || *argv[2] == '\0' || *endptr != '\0' ||
	    msg.pvt.longitude < -180.0 || msg.pvt.longitude > 180.0) {
		shell_print(sh, "Invalid longitude. Expected a value in range [-180, 180]");
		return 1;
	}

	errno = 0;
	msg.pvt.altitude = (float)strtod(argv[3], &endptr);
	if (errno != 0 || *argv[3] == '\0' || *endptr != '\0') {
		shell_print(sh, "Invalid altitude. Expected a numeric value in meters");
		return 1;
	}

	errno = 0;
	msg.location_validity_seconds = (uint32_t)strtoul(argv[4], &endptr, 10);
	if (errno != 0 || *argv[4] == '\0' || *endptr != '\0') {
		shell_print(sh, "Invalid validity. Expected a value in seconds");
		return 1;
	}

	err = zbus_chan_pub(&NTN_CHAN, &msg, K_SECONDS(1));
	if (err) {
		shell_print(sh, "Failed to publish GNSS location message, error: %d", err);
		return 1;
	}

	shell_print(sh, "Injected GNSS location: lat=%.6f lon=%.6f alt=%.2f m",
		    msg.pvt.latitude, msg.pvt.longitude, (double)msg.pvt.altitude);
	return 0;
}

static int cmd_set_datetime_manual(const struct shell *sh, size_t argc, char **argv)
{
	int err;
	struct ntn_msg msg = {
		.type = NTN_SHELL_SET_DATETIME,
	};
	struct tm parsed_time;

	if (argc != 2) {
		shell_print(sh, "Usage: att_ntn set_datetime <YYYY-MM-DD-HH:MM:SS>");
		shell_print(sh, "Example: att_ntn set_datetime \"2026-02-11-10:00:00\"");
		return 1;
	}

	memset(&parsed_time, 0, sizeof(parsed_time));
	if (strptime(argv[1], "%Y-%m-%d-%H:%M:%S", &parsed_time) == NULL) {
		shell_print(sh, "Invalid date time format. Use: YYYY-MM-DD-HH:MM:SS");
		return 1;
	}

	strncpy(msg.datetime, argv[1], sizeof(msg.datetime) - 1);
	msg.datetime[sizeof(msg.datetime) - 1] = '\0';

	err = zbus_chan_pub(&NTN_CHAN, &msg, K_SECONDS(1));
	if (err) {
		shell_print(sh, "Failed to publish date time message, error: %d", err);
		return 1;
	}

	shell_print(sh, "Setting manual date time to: %s", msg.datetime);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_att_ntn,
	SHELL_CMD(ntn_trigger, NULL, "Trigger NTN state manually", cmd_ntn_trigger),
	SHELL_CMD(gnss_trigger, NULL, "Trigger GNSS manually", cmd_gnss_trigger),
	SHELL_CMD(idle_trigger, NULL, "Trigger IDLE state manually", cmd_idle_trigger),
	SHELL_CMD(set_gnss_location, NULL, "Inject GNSS location without running GNSS",
		  cmd_set_gnss_location_manual),
	SHELL_CMD(set_datetime, NULL, "Set date time manually (format: YYYY-MM-DD-HH:MM:SS)",
		  cmd_set_datetime_manual),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(att_ntn, &sub_att_ntn, "NTN commands", NULL);
