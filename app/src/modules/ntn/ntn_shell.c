/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#define _XOPEN_SOURCE

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <time.h>

#include "ntn.h"

LOG_MODULE_DECLARE(ntn, CONFIG_APP_NTN_LOG_LEVEL);

static bool parse_datetime_arg(const char *datetime_str, struct tm *parsed_time)
{
	char *end;

	memset(parsed_time, 0, sizeof(*parsed_time));
	end = strptime(datetime_str, "%Y-%m-%d-%H:%M:%S", parsed_time);

	return end != NULL && *end == '\0';
}

static int cmd_ntn_trigger(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct ntn_msg msg = {
		.type = NTN_TRIGGER
	};

	int err = zbus_chan_pub(&NTN_CHAN, &msg, K_SECONDS(1));
	if (err) {
		shell_print(sh, "Failed to publish ntn trigger message, error: %d", err);
		return 1;
	}

	shell_print(sh, "Triggering NTN state manually");
	return 0;
}

static int cmd_sgp4_trigger(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct ntn_msg msg = {
		.type = RUN_SGP4
	};

	int err = zbus_chan_pub(&NTN_CHAN, &msg, K_SECONDS(1));
	if (err) {
		shell_print(sh, "Failed to publish SGP4 message, error: %d", err);
		return 1;
	}

	shell_print(sh, "Triggering SGP4 manually");
	return 0;
}

static int cmd_gnss_trigger(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct ntn_msg msg = {
		.type = GNSS_TRIGGER
	};

	int err = zbus_chan_pub(&NTN_CHAN, &msg, K_SECONDS(1));
	if (err) {
		shell_print(sh, "Failed to publish GNSS message, error: %d", err);
		return 1;
	}

	shell_print(sh, "Triggering GNSS manually");
	return 0;
}

static int cmd_set_gnss_location_manual(const struct shell *sh, size_t argc, char **argv)
{
	int err;
	char *endptr;
	struct ntn_msg msg = {
		.type = NTN_SHELL_SET_GNSS_LOCATION,
	};

	if (argc != 4) {
		shell_print(sh, "Usage: att_ntn set_gnss_location <lat> <lon> <alt_m>");
		shell_print(sh, "Example: att_ntn set_gnss_location 57.7089 11.9746 15.0");
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

	err = zbus_chan_pub(&NTN_CHAN, &msg, K_SECONDS(1));
	if (err) {
		shell_print(sh, "Failed to publish GNSS location message, error: %d", err);
		return 1;
	}

	shell_print(sh, "Injected GNSS location: lat=%.6f lon=%.6f alt=%.2f m",
		    msg.pvt.latitude, msg.pvt.longitude, (double)msg.pvt.altitude);
	return 0;
}

static int cmd_set_tle_manual(const struct shell *sh, size_t argc, char **argv)
{
	int err;
	struct ntn_msg msg = {
		.type = NTN_SHELL_SET_TLE,
	};

	if (argc != 4) {
		shell_print(sh, "Usage: att_ntn set_tle \"<name>\" \"<line1>\" \"<line2>\"");
		shell_print(sh, "Example: att_ntn set_tle \"SIOT1\" "
			    "\"1 12345U 24001A   26077.50000000  .00000000  00000-0  00000-0 0  9991\" "
			    "\"2 12345  86.4000 180.0000 0001000   0.0000 180.0000 14.20000000    01\"");
		return 1;
	}

	if (argv[1][0] == '\0') {
		shell_print(sh, "Invalid TLE name. It must not be empty");
		return 1;
	}

	if (argv[2][0] != '1' || argv[3][0] != '2') {
		shell_print(sh, "Invalid TLE lines. Line1 must start with '1' and line2 with '2'");
		return 1;
	}

	if (strlen(argv[1]) >= sizeof(msg.tle_name) ||
	    strlen(argv[2]) >= sizeof(msg.tle_line1) ||
	    strlen(argv[3]) >= sizeof(msg.tle_line2)) {
		shell_print(sh, "TLE input too long");
		return 1;
	}

	strncpy(msg.tle_name, argv[1], sizeof(msg.tle_name) - 1);
	strncpy(msg.tle_line1, argv[2], sizeof(msg.tle_line1) - 1);
	strncpy(msg.tle_line2, argv[3], sizeof(msg.tle_line2) - 1);

	err = zbus_chan_pub(&NTN_CHAN, &msg, K_SECONDS(1));
	if (err) {
		shell_print(sh, "Failed to publish TLE message, error: %d", err);
		return 1;
	}

	shell_print(sh, "Provisioned manual TLE for: %s", msg.tle_name);
	return 0;
}

static int cmd_set_time_of_pass_manual(const struct shell *sh, size_t argc, char **argv)
{
	int err;
	struct ntn_msg msg = {
		.type = NTN_SHELL_SET_TIME,
	};
	struct tm parsed_time;

	if (argc != 2) {
		shell_print(sh, "Usage: att_ntn set_time_of_pass <YYYY-MM-DD-HH:MM:SS>");
		shell_print(sh, "Example: att_ntn set_time_of_pass \"2025-10-07-14:30:00\"");
		return 1;
	}

	if (!parse_datetime_arg(argv[1], &parsed_time)) {
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

static int cmd_set_datetime_manual(const struct shell *sh, size_t argc, char **argv)
{
	int err;
	struct ntn_msg msg = {
		.type = NTN_SHELL_SET_DATETIME,
	};
	struct tm parsed_time;

	if (argc != 2) {
		shell_print(sh, "Usage: att_ntn set_datetime <YYYY-MM-DD-HH:MM:SS>");
		shell_print(sh, "Example: att_ntn set_datetime \"2025-10-07-14:30:00\"");
		return 1;
	}

	if (!parse_datetime_arg(argv[1], &parsed_time)) {
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
	SHELL_CMD(sgp4_trigger, NULL, "Trigger SGP4 manually", cmd_sgp4_trigger),
	SHELL_CMD(gnss_trigger, NULL, "Trigger GNSS manually", cmd_gnss_trigger),
	SHELL_CMD(set_gnss_location, NULL, "Inject GNSS location without running GNSS", cmd_set_gnss_location_manual),
	SHELL_CMD(set_tle, NULL, "Provision TLE manually without fetching", cmd_set_tle_manual),
	SHELL_CMD(set_time_of_pass, NULL, "Set new time of pass (format: YYYY-MM-DD-HH:MM:SS)", cmd_set_time_of_pass_manual),
	SHELL_CMD(set_datetime, NULL, "Set date time manually (format: YYYY-MM-DD-HH:MM:SS)", cmd_set_datetime_manual),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(att_ntn, &sub_att_ntn, "NTN commands", NULL);
