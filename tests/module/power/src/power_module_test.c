/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <unity.h>
#include <zephyr/fff.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/logging/log.h>
#include <date_time.h>

#include "app_common.h"
#include "power.h"

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);
FAKE_VALUE_FUNC(float, nrf_fuel_gauge_process, float, float, float, float, bool, void *);
FAKE_VALUE_FUNC(int, charger_read_sensors, float *, float *, float *, int32_t *);
FAKE_VALUE_FUNC(int, nrf_fuel_gauge_init, const struct nrf_fuel_gauge_init_parameters *, void *);
FAKE_VALUE_FUNC(int, mfd_npm1300_add_callback, const struct device *, struct gpio_callback *);
FAKE_VALUE_FUNC(int, date_time_now, int64_t *);

ZBUS_MSG_SUBSCRIBER_DEFINE(power_subscriber);
ZBUS_CHAN_ADD_OBS(POWER_CHAN, power_subscriber, 0);

LOG_MODULE_REGISTER(power_module_test, 4);

/* Test timestamp value to be returned by date_time_now mock */
static int64_t test_timestamp = 1640995200000; /* 2022-01-01 00:00:00 UTC in ms */

static int date_time_now_custom_fake(int64_t *timestamp)
{
	*timestamp = test_timestamp;
	return 0;
}

static int date_time_now_error_fake(int64_t *timestamp)
{
	ARG_UNUSED(timestamp);

	/* Return error, timestamp should remain 0 */
	return -ENODATA;
}

void setUp(void)
{
	/* reset fakes */
	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(task_wdt_add);
	RESET_FAKE(date_time_now);

	/* Set up custom fake function for date_time_now */
	date_time_now_fake.custom_fake = date_time_now_custom_fake;
}

void check_power_event(enum power_msg_type expected_power_type)
{
	int err;
	const struct zbus_channel *chan;
	struct power_msg power_msg;

	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_MSEC(100));

	err = zbus_sub_wait_msg(&power_subscriber, &chan, &power_msg, K_MSEC(1000));
	if (err == -ENOMSG) {
		LOG_ERR("No power event received");
		TEST_FAIL();
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	if (chan != &POWER_CHAN) {
		LOG_ERR("Received message from wrong channel");
		TEST_FAIL();
	}

	TEST_ASSERT_EQUAL(expected_power_type, power_msg.type);

	/* Verify timestamp for response messages */
	if (expected_power_type == POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE) {
		TEST_ASSERT_EQUAL(test_timestamp, power_msg.timestamp);
		LOG_DBG("Timestamp verification passed: %lld", power_msg.timestamp);
	}
}

void check_no_power_events(uint32_t time_in_seconds)
{
	int err;
	const struct zbus_channel *chan;
	struct power_msg power_msg;

	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_SECONDS(time_in_seconds));

	err = zbus_sub_wait_msg(&power_subscriber, &chan, &power_msg, K_MSEC(1000));
	if (err == 0) {
		LOG_ERR("Received trigger event with type %d", power_msg.type);
		TEST_FAIL();
	}
}

static void send_power_battery_percentage_sample_request(void)
{
	struct power_msg msg = {
		.type = POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST,
	};

	int err = zbus_chan_pub(&POWER_CHAN, &msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

void test_timestamp_verification(void)
{
	/* Test with different timestamp values */
	int64_t test_timestamps[] = {
		1640995200000, /* 2022-01-01 00:00:00 UTC in ms */
		1672531200000, /* 2023-01-01 00:00:00 UTC in ms */
		1704067200000  /* 2024-01-01 00:00:00 UTC in ms */
	};

	for (int i = 0; i < ARRAY_SIZE(test_timestamps); i++) {
		/* Set new timestamp value */
		test_timestamp = test_timestamps[i];

		/* Given */
		send_power_battery_percentage_sample_request();

		/* When */
		k_sleep(K_SECONDS(1));

		/* Then */
		check_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
		check_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE);

		/* Verify that date_time_now was called */
		TEST_ASSERT_GREATER_THAN(0, date_time_now_fake.call_count);

		/* Reset for next iteration */
		RESET_FAKE(date_time_now);
		date_time_now_fake.custom_fake = date_time_now_custom_fake;
	}
}

void test_timestamp_error_handling(void)
{
	/* Test error handling when date_time_now fails */

	/* Set up error fake */
	date_time_now_fake.custom_fake = date_time_now_error_fake;

	/* Given */
	send_power_battery_percentage_sample_request();

	/* When */
	k_sleep(K_SECONDS(1));

	/* Then */
	check_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);

	/* For error case, we expect timestamp to be 0 */
	int err;
	const struct zbus_channel *chan;
	struct power_msg power_msg;

	k_sleep(K_MSEC(100));

	err = zbus_sub_wait_msg(&power_subscriber, &chan, &power_msg, K_MSEC(1000));
	TEST_ASSERT_EQUAL(0, err);
	TEST_ASSERT_EQUAL(POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE, power_msg.type);
	TEST_ASSERT_EQUAL(0, power_msg.timestamp); /* Should be 0 on error */

	/* Verify that date_time_now was called */
	TEST_ASSERT_GREATER_THAN(0, date_time_now_fake.call_count);
}

void test_power_percentage_sample(void)
{
	for (int i = 0; i < 10; i++) {
		/* Given */
		send_power_battery_percentage_sample_request();

		/* When */
		k_sleep(K_SECONDS(1));

		/* Then */
		check_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
		check_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE);
		check_no_power_events(3600);
	}
}

/* This is required to be added to each test. That is because unity's
 * main may return nonzero, while zephyr's main currently must
 * return 0 in all cases (other values are reserved).
 */
extern int unity_main(void);

int main(void)
{
	(void)unity_main();

	k_sleep(K_FOREVER);

	return 0;
}
