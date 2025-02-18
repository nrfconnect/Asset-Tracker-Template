/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <unity.h>
#include <zephyr/fff.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/logging/log.h>

#include "message_channel.h"

DEFINE_FFF_GLOBALS;

LOG_MODULE_REGISTER(shell_test_main, LOG_LEVEL_DBG);

void setUp(void)
{
	// This function is run before each test
}

void tearDown(void)
{
	// This function is run after each test
}

extern void test_valid_zbus_channel_valid_payload(void);
extern void test_invalid_zbus_channel(void);
extern void test_invalid_payload_format(void);

int main(void)
{
	UNITY_BEGIN();

	RUN_TEST(test_valid_zbus_channel_valid_payload);
	RUN_TEST(test_invalid_zbus_channel);
	RUN_TEST(test_invalid_payload_format);

	return UNITY_END();
}
