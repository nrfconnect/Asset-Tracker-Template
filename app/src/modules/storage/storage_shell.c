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

static int cmd_storage_fifo_request(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct storage_msg msg = { .type = STORAGE_FIFO_REQUEST };
	int err;

	err = zbus_chan_pub(&STORAGE_CHAN, &msg, K_SECONDS(1));
	if (err) {
		shell_error(sh, "Failed to publish STORAGE_FIFO_REQUEST: %d", err);
		return err;
	}

	shell_print(sh, "Storage FIFO request initiated.");
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

static int cmd_storage_fifo_clear(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct storage_msg msg = { .type = STORAGE_FIFO_CLEAR };
	int err;

	err = zbus_chan_pub(&STORAGE_CHAN, &msg, K_SECONDS(1));
	if (err) {
		shell_error(sh, "Failed to publish STORAGE_FIFO_CLEAR: %d", err);
		return err;
	}

	shell_print(sh, "Storage FIFO clear initiated.");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(storage_sub_cmds,
	SHELL_CMD(flush, NULL, "Flush stored data", cmd_storage_flush),
	SHELL_CMD(fifo_request, NULL, "Request data from FIFO", cmd_storage_fifo_request),
	SHELL_CMD(clear, NULL, "Clear all stored data", cmd_storage_clear),
	SHELL_CMD(fifo_clear, NULL, "Clear FIFO data", cmd_storage_fifo_clear),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(storage, &storage_sub_cmds, "Storage module commands", NULL);
