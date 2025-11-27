/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <unity.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/fff.h>

#include "app_common.h"

DEFINE_FFF_GLOBALS;

static K_SEM_DEFINE(assert_triggered_sem, 0, 1);

extern void assert_post_action(const char *file, unsigned int line);

void assert_post_action(const char *file, unsigned int line)
{
	ARG_UNUSED(file);
	ARG_UNUSED(line);

	k_sem_give(&assert_triggered_sem);
}

void test_fatal_error_handler(void)
{
	int err;

	SEND_FATAL_ERROR();

	err = k_sem_take(&assert_triggered_sem, K_SECONDS(10));
	TEST_ASSERT_EQUAL(0, err);

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();

	err = k_sem_take(&assert_triggered_sem, K_SECONDS(10));
	TEST_ASSERT_EQUAL(0, err);
}

/* This is required to be added to each test. That is because unity's
 * main may return nonzero, while zephyr's main currently must
 * return 0 in all cases (other values are reserved).
 */
extern int unity_main(void);

int main(void)
{
	/* use the runner from test_runner_generate() */
	(void)unity_main();

	return 0;
}
