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
#include "sgp4_pass_predict.h"

LOG_MODULE_DECLARE(ntn, CONFIG_APP_NTN_LOG_LEVEL);

static bool parse_datetime_arg(const char *datetime_str, struct tm *parsed_time)
{
	char *end;

	memset(parsed_time, 0, sizeof(*parsed_time));
	end = strptime(datetime_str, "%Y-%m-%d-%H:%M:%S", parsed_time);

	return end != NULL && *end == '\0';
}

static bool normalize_sib32_arg(const char *input, char *output, size_t output_size)
{
	const char *sib32;
	size_t len;

	if (input == NULL || output == NULL || output_size == 0) {
		return false;
	}

	sib32 = strstr(input, "SIBCONFIG:");
	if (sib32 == NULL) {
		sib32 = input;
	}

	len = strlen(sib32);
	if (len == 0 || len >= output_size) {
		return false;
	}

	strncpy(output, sib32, output_size - 1);
	output[output_size - 1] = '\0';

	return strncmp(output, "SIBCONFIG:", strlen("SIBCONFIG:")) == 0;
}

static void print_set_tle_help(const struct shell *sh)
{
	shell_print(sh, "Usage: att_ntn set_tle \"<name>\" \"<line1>\" \"<line2>\"");
	shell_print(sh, "Example 1: att_ntn set_tle \"SATELIOT_1\" "
		    "\"1 60550U 24149CL  26041.62762187  .00003124  00000+0  27307-3 0  9999\" "
		    "\"2 60550  97.6859 119.6343 0008059 130.5773 229.6152 14.97467371 81125\"");
	shell_print(sh, "Example 2: att_ntn set_tle \"SATELIOT_3\" "
			"\"1 60552U 24149CN  26041.67052983  .00002578  00000+0  22594-3 0  9997\" "
			"\"2 60552  97.6924 120.3514 0005931 143.3246 216.8383 14.97526427 81146\"");
	shell_print(sh, "Repeat up to 4 times to provision multiple satellites");
}

static void print_set_sib32_help(const struct shell *sh)
{
	shell_print(sh, "Usage: att_ntn set_sib32 \"<SIBCONFIG: 32,...>\"");
	shell_print(sh, "Example: att_ntn set_sib32 "
		    "\"SIBCONFIG: 32,\\\"00000001\\\",2,1,1138123,1529334,1391197,758633,13719,2572629918,28139,-3,120679,,11,11,,,,3,1138188,1686202,1399132,2534648,10028,2572728485,19572,-3,121275,,11,11,,,\"");
}

static void print_sgp4_trigger_help(const struct shell *sh)
{
	shell_print(sh, "Usage: att_ntn sgp4_trigger [min_elevation_deg]");
	shell_print(sh, "Example: att_ntn sgp4_trigger 50");
	shell_print(sh, "Default minimum elevation is %.1f degrees",
		    SGP4_DEFAULT_MIN_ELEVATION_DEG);
}

static void print_set_peak_offset_help(const struct shell *sh)
{
	shell_print(sh, "Usage: att_ntn set_peak_offset <seconds>");
	shell_print(sh, "Example: att_ntn set_peak_offset 45");
	shell_print(sh, "Positive values trigger before peak, negative values after peak");
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
	char *endptr;
	double min_elevation_deg = SGP4_DEFAULT_MIN_ELEVATION_DEG;

	if (argc > 2) {
		print_sgp4_trigger_help(sh);
		return 1;
	}

	if (argc == 2) {
		errno = 0;
		min_elevation_deg = strtod(argv[1], &endptr);
		if (errno != 0 || *argv[1] == '\0' || *endptr != '\0' ||
		    min_elevation_deg < 0.0 || min_elevation_deg > 90.0) {
			shell_print(sh,
				    "Invalid minimum elevation. Expected a value in range [0, 90]");
			return 1;
		}
	}

	struct ntn_msg msg = {
		.type = SGP4_TRIGGER,
		.sgp4_min_elevation_deg = (float)min_elevation_deg,
	};

	int err = zbus_chan_pub(&NTN_CHAN, &msg, K_SECONDS(1));
	if (err) {
		shell_print(sh, "Failed to publish SGP4 message, error: %d", err);
		return 1;
	}

	shell_print(sh, "Triggering SGP4 manually with minimum elevation %.2f degrees",
		    min_elevation_deg);
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

static int cmd_idle_trigger(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct ntn_msg msg = {
		.type = IDLE_TRIGGER
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

	if (argc != 4) {
		shell_print(sh, "Usage: att_ntn set_gnss_location <lat> <lon> <alt_m>");
		shell_print(sh, "Example: att_ntn set_gnss_location 63.43 10.39 40.0");
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

	if (argc != 4 || argv[1][0] == '\0' || argv[2][0] == '\0' || argv[3][0] == '\0') {
		print_set_tle_help(sh);
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

static int cmd_set_sib32_manual(const struct shell *sh, size_t argc, char **argv)
{
	int err;
	struct ntn_msg msg = {
		.type = NTN_SET_SIB32,
	};

	if (argc != 2 || argv[1][0] == '\0') {
		print_set_sib32_help(sh);
		return 1;
	}

	if (!normalize_sib32_arg(argv[1], msg.sib32_data, sizeof(msg.sib32_data))) {
		shell_print(sh, "Invalid SIB32 payload. Expected a SIBCONFIG: 32 notification string");
		return 1;
	}

	err = zbus_chan_pub(&NTN_CHAN, &msg, K_SECONDS(1));
	if (err) {
		shell_print(sh, "Failed to publish SIB32 message, error: %d", err);
		return 1;
	}

	shell_print(sh, "Provisioned manual SIB32 prediction data");
	return 0;
}

static int cmd_clear_sib32_manual(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argv);

	if (argc != 1) {
		shell_print(sh, "Usage: att_ntn clear_sib32");
		return 1;
	}

	struct ntn_msg msg = {
		.type = NTN_CLEAR_SIB32,
	};
	int err = zbus_chan_pub(&NTN_CHAN, &msg, K_SECONDS(1));

	if (err) {
		shell_print(sh, "Failed to publish clear SIB32 message, error: %d", err);
		return 1;
	}

	shell_print(sh, "Cleared cached SIB32 prediction data");
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
		shell_print(sh, "Example: att_ntn set_time_of_pass \"2026-02-11-10:00:00\"");
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
		shell_print(sh, "Example: att_ntn set_datetime \"2026-02-11-10:00:00\"");
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

static int cmd_set_peak_offset_manual(const struct shell *sh, size_t argc, char **argv)
{
	int err;
	char *endptr;
	long peak_offset_seconds;
	struct ntn_msg msg = {
		.type = NTN_SHELL_SET_PEAK_OFFSET,
	};

	if (argc != 2) {
		print_set_peak_offset_help(sh);
		return 1;
	}

	errno = 0;
	peak_offset_seconds = strtol(argv[1], &endptr, 10);
	if (errno != 0 || *argv[1] == '\0' || *endptr != '\0' ||
	    peak_offset_seconds < INT32_MIN || peak_offset_seconds > INT32_MAX) {
		shell_print(sh, "Invalid peak offset. Expected a signed 32-bit integer in seconds");
		return 1;
	}

	msg.peak_offset_seconds = (int32_t)peak_offset_seconds;

	err = zbus_chan_pub(&NTN_CHAN, &msg, K_SECONDS(1));
	if (err) {
		shell_print(sh, "Failed to publish peak offset message, error: %d", err);
		return 1;
	}

	shell_print(sh, "Setting NTN peak-time offset to: %ld seconds", peak_offset_seconds);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_att_ntn,
	SHELL_CMD(ntn_trigger, NULL, "Trigger NTN state manually", cmd_ntn_trigger),
	SHELL_CMD(sgp4_trigger, NULL,
		  "Trigger SGP4 manually [min_elevation_deg]",
		  cmd_sgp4_trigger),
	SHELL_CMD(gnss_trigger, NULL, "Trigger GNSS manually", cmd_gnss_trigger),
	SHELL_CMD(idle_trigger, NULL, "Trigger IDLE state manually", cmd_idle_trigger),
	SHELL_CMD(set_gnss_location, NULL, "Inject GNSS location without running GNSS", cmd_set_gnss_location_manual),
	SHELL_CMD(set_tle, NULL, "Provision one TLE manually; repeat up to 4 satellites", cmd_set_tle_manual),
	SHELL_CMD(set_sib32, NULL, "Provision raw SIB32 prediction data", cmd_set_sib32_manual),
	SHELL_CMD(clear_sib32, NULL, "Clear cached SIB32 so prediction can fall back to TLE", cmd_clear_sib32_manual),
	SHELL_CMD(set_peak_offset, NULL, "Override NTN peak-time offset in seconds", cmd_set_peak_offset_manual),
	SHELL_CMD(set_time_of_pass, NULL, "Set new time of pass (format: YYYY-MM-DD-HH:MM:SS)", cmd_set_time_of_pass_manual),
	SHELL_CMD(set_datetime, NULL, "Set date time manually (format: YYYY-MM-DD-HH:MM:SS)", cmd_set_datetime_manual),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(att_ntn, &sub_att_ntn, "NTN commands", NULL);
