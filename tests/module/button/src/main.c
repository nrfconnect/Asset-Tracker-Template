/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <unity.h>
#include <zephyr/fff.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/logging/log.h>
#include <dk_buttons_and_leds.h>
#include <date_time.h>

#include "app_common.h"
#include "button.h"

DEFINE_FFF_GLOBALS;

LOG_MODULE_REGISTER(button_module_test, 4);

ZBUS_MSG_SUBSCRIBER_DEFINE(button_subscriber);
ZBUS_CHAN_ADD_OBS(BUTTON_CHAN, button_subscriber, 0);

#define FAKE_TIME_MS 1716552398505

int date_time_now(int64_t *time)
{
	*time = FAKE_TIME_MS;
	return 0;
}

static button_handler_t button_handler = NULL;

int dk_buttons_init(button_handler_t _button_handler)
{
	button_handler = _button_handler;
	return 0;
}

void tearDown(void)
{
	const struct zbus_channel *chan;
	uint8_t button_number;
	int err;

	err = zbus_sub_wait_msg(&button_subscriber, &chan, &button_number, K_MSEC(1000));
	if (err == 0) {
		LOG_ERR("Unhandled message in payload channel");
		TEST_FAIL();
	}
}

void test_button_trigger(void)
{
	const struct zbus_channel *chan;
	uint8_t button_number;
	int err;

	TEST_ASSERT_NOT_NULL(button_handler);
	button_handler(DK_BTN1_MSK, DK_BTN1_MSK);

	err = zbus_sub_wait_msg(&button_subscriber, &chan, &button_number, K_MSEC(1000));
	if (err == -ENOMSG) {
		LOG_ERR("No BUTTON_CHAN message received");
		TEST_FAIL();
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	/* check if chan is button channel */
	if (chan != &BUTTON_CHAN) {
		LOG_ERR("Received message from wrong channel");
		TEST_FAIL();
	}
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
