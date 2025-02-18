/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/ztest.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include "message_channel.h"

LOG_MODULE_REGISTER(shell_test, LOG_LEVEL_DBG);

extern int cmd_publish_on_payload_chan(const struct shell *sh, size_t argc, char **argv);

static const struct shell *shell_global;

static void test_valid_zbus_channel_valid_payload(void)
{
	const char *argv[] = {"zbus", "payload_chan", "test_payload"};
	int argc = ARRAY_SIZE(argv);

	int ret = cmd_publish_on_payload_chan(shell_global, argc, (char **)argv);
	zassert_equal(ret, 0, "Expected success for valid zbus channel and valid payload");
}

static void test_invalid_zbus_channel(void)
{
	const char *argv[] = {"zbus", "invalid_chan", "test_payload"};
	int argc = ARRAY_SIZE(argv);

	int ret = cmd_publish_on_payload_chan(shell_global, argc, (char **)argv);
	zassert_not_equal(ret, 0, "Expected failure for invalid zbus channel");
}

static void test_invalid_payload_format(void)
{
	const char *argv[] = {"zbus", "payload_chan", ""};
	int argc = ARRAY_SIZE(argv);

	int ret = cmd_publish_on_payload_chan(shell_global, argc, (char **)argv);
	zassert_not_equal(ret, 0, "Expected failure for invalid payload format");
}

void test_main(void)
{
	ztest_test_suite(shell_tests,
		ztest_unit_test(test_valid_zbus_channel_valid_payload),
		ztest_unit_test(test_invalid_zbus_channel),
		ztest_unit_test(test_invalid_payload_format)
	);

	ztest_run_test_suite(shell_tests);
}
