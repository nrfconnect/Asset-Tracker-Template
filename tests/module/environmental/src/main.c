/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <unity.h>

#include <zephyr/fff.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/logging/log.h>

#include "message_channel.h"
#include "environmental.h"

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);

ZBUS_MSG_SUBSCRIBER_DEFINE(environmental_subscriber);
ZBUS_CHAN_ADD_OBS(ENVIRONMENTAL_CHAN, environmental_subscriber, 0);

LOG_MODULE_REGISTER(environmental_module_test, 4);

void setUp(void)
{
	/* reset fakes */
	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(task_wdt_add);
}

void check_environmental_event(enum environmental_msg_type expected_environmental_type)
{
	int err;
	const struct zbus_channel *chan;
	struct environmental_msg environmental_msg;

	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_MSEC(100));

	err = zbus_sub_wait_msg(&environmental_subscriber, &chan, &environmental_msg, K_MSEC(1000));
	if (err == -ENOMSG) {
		LOG_ERR("No envornmental event received");
		TEST_FAIL();
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	if (chan != &ENVIRONMENTAL_CHAN) {
		LOG_ERR("Received message from wrong channel");
		TEST_FAIL();
	}

	TEST_ASSERT_EQUAL(expected_environmental_type, environmental_msg.type);
}

void check_no_environmental_events(uint32_t time_in_seconds)
{
	int err;
	const struct zbus_channel *chan;
	struct environmental_msg environmental_msg;

	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_SECONDS(time_in_seconds));

	err = zbus_sub_wait_msg(&environmental_subscriber, &chan, &environmental_msg, K_MSEC(1000));
	if (err == 0) {
		LOG_ERR("Received trigger event with type %d", environmental_msg.type);
		TEST_FAIL();
	}
}

static void send_environmental_sample_request(void)
{
	struct environmental_msg msg = {
		.type = ENVIRONMENTAL_SENSOR_SAMPLE_REQUEST,
	};

	int err = zbus_chan_pub(&ENVIRONMENTAL_CHAN, &msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

void test_sensor_sample(void)
{
	for (int i = 0; i < 10; i++) {
		/* Given */
		send_environmental_sample_request();

		/* When */
		k_sleep(K_SECONDS(1));

		/* Then */
		check_environmental_event(ENVIRONMENTAL_SENSOR_SAMPLE_REQUEST);
		check_environmental_event(ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE);
		check_no_environmental_events(3600);
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
