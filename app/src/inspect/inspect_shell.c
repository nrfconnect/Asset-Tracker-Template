/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/iterable_sections.h>

#include <errno.h>
#include <strings.h>

#include "app_inspect.h"

/**
 * @brief Format string for printing module state in inspect shell.
 */
#define INSPECT_PRINT_FMT "%-14s | %s"

static int cmd_att_inspect(const struct shell *sh, size_t argc, char **argv)
{
	STRUCT_SECTION_FOREACH(app_inspect_provider, provider) {
		const char *state_name = "STATE_UNKNOWN";

		if ((provider->get_state_name != NULL)) {
			state_name = provider->get_state_name();
		}

		shell_print(sh, INSPECT_PRINT_FMT, provider->name, state_name);
	}

	return 0;
}

SHELL_CMD_REGISTER(att_inspect, NULL, "Inspect module SMF states: att_inspect", cmd_att_inspect);
