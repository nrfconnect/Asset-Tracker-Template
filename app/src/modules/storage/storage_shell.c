/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include "storage.h"
#include "storage_data_types.h" /* For STORAGE_CHAN */

LOG_MODULE_REGISTER(storage_shell, CONFIG_APP_STORAGE_LOG_LEVEL);

static int cmd_storage_flush(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct storage_msg msg = { .type = STORAGE_FLUSH };
	int err;

	err = zbus_chan_pub(&STORAGE_CHAN, &msg, K_SECONDS(1));
	if (err) {
		shell_error(sh, "Failed to publish STORAGE_FLUSH: %d", err);
		return err;
	}

	shell_print(sh, "Storage flush initiated.");
	return 0;
}

static int cmd_storage_batch_request(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct storage_msg msg = { .type = STORAGE_BATCH_REQUEST, .session_id = 0x12345678 };
	int err;

	err = zbus_chan_pub(&STORAGE_CHAN, &msg, K_SECONDS(1));
	if (err) {
		shell_error(sh, "Failed to publish STORAGE_BATCH_REQUEST: %d", err);
		return err;
	}

	shell_print(sh, "Storage batch request initiated.");

	return 0;
}

static int cmd_storage_clear(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct storage_msg msg = { .type = STORAGE_CLEAR };
	int err;

	err = zbus_chan_pub(&STORAGE_CHAN, &msg, K_SECONDS(1));
	if (err) {
		shell_error(sh, "Failed to publish STORAGE_CLEAR: %d", err);
		return err;
	}

	shell_print(sh, "Storage clear initiated.");
	return 0;
}

static int cmd_storage_batch_close(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct storage_msg msg = { .type = STORAGE_BATCH_CLOSE, .session_id = 0x12345678 };
	int err;

	err = zbus_chan_pub(&STORAGE_CHAN, &msg, K_SECONDS(1));
	if (err) {
		shell_error(sh, "Failed to publish STORAGE_BATCH_CLOSE: %d", err);
		return err;
	}

	shell_print(sh, "Storage batch close initiated.");
	return 0;
}

static int cmd_storage_stats(const struct shell *sh, size_t argc, char **argv)
{
#if IS_ENABLED(CONFIG_APP_STORAGE_SHELL_STATS)
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct storage_msg msg = { .type = STORAGE_STATS };
	int err;

	err = zbus_chan_pub(&STORAGE_CHAN, &msg, K_SECONDS(1));
	if (err) {
		shell_error(sh, "Failed to publish STORAGE_STATS: %d", err);
		return err;
	}

	shell_print(sh, "Storage statistics request initiated.");
#else
	shell_error(sh, "Storage statistics command is not enabled in the shell.");
#endif /* CONFIG_APP_STORAGE_SHELL_STATS */

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_cmds,
	SHELL_CMD(flush, NULL, "Flush stored data", cmd_storage_flush),
	SHELL_CMD(batch_request, NULL, "Request data from batch", cmd_storage_batch_request),
	SHELL_CMD(clear, NULL, "Clear all stored data", cmd_storage_clear),
	SHELL_CMD(batch_close, NULL, "Close batch session", cmd_storage_batch_close),
	SHELL_CMD(stats, NULL, "Show storage statistics", cmd_storage_stats),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(att_storage,
		   &sub_cmds,
		   "Asset Tracker Template Storage module commands",
		   NULL);
