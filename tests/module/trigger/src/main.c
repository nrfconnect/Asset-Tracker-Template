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
#include "dk_buttons_and_leds.h"
#include "message_channel.h"

#define FREQUENT_POLL_TRIGGER_INTERVAL_SEC 60
#define FREQUENT_POLL_SHADOW_POLL_TRIGGER_INTERVAL_SEC 30

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(int, dk_buttons_init, button_handler_t);
FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);

LOG_MODULE_REGISTER(trigger_module_test, 4);

ZBUS_MSG_SUBSCRIBER_DEFINE(trig_mode_subscriber);
ZBUS_CHAN_ADD_OBS(TRIGGER_MODE_CHAN, trig_mode_subscriber, 0);

ZBUS_MSG_SUBSCRIBER_DEFINE(trigger_subscriber);
ZBUS_CHAN_ADD_OBS(TRIGGER_CHAN, trigger_subscriber, 0);

void setUp(void)
{
	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(task_wdt_add);
}

static void send_cloud_connected_ready_to_send(void)
{
	enum cloud_status status = CLOUD_CONNECTED_READY_TO_SEND;
	int err = zbus_chan_pub(&CLOUD_CHAN, &status, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_config(void)
{
#define TEST_UPDATE_INTERVAL_IN_SECONDS 3600

	const struct configuration config = {
		.update_interval_present = true,
		.update_interval = TEST_UPDATE_INTERVAL_IN_SECONDS,
	};
	int err = zbus_chan_pub(&CONFIG_CHAN, &config, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_cloud_disconnected(void)
{
	enum cloud_status status = CLOUD_DISCONNECTED;
	int err = zbus_chan_pub(&CLOUD_CHAN, &status, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_location_event(enum location_status location_status)
{
	int err = zbus_chan_pub(&LOCATION_CHAN, &location_status, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void check_trigger_mode_event(enum trigger_mode expected_trigger_mode)
{
	const struct zbus_channel *chan;
	enum trigger_mode trigger_mode;
	int err;

	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_MSEC(100));

	err = zbus_sub_wait_msg(&trig_mode_subscriber, &chan, &trigger_mode, K_MSEC(1000));
	if (err == -ENOMSG) {
		LOG_ERR("No trigger mode received");
		TEST_FAIL();
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	if (chan != &TRIGGER_MODE_CHAN) {
		LOG_ERR("Received message from wrong channel");
		TEST_FAIL();
	}

	TEST_ASSERT_EQUAL(expected_trigger_mode, trigger_mode);
}

static void check_trigger_event(enum trigger_type expected_trig_type)
{
	const struct zbus_channel *chan;
	enum trigger_type trig_type;
	int err;

	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_MSEC(100));

	err = zbus_sub_wait_msg(&trigger_subscriber, &chan, &trig_type, K_MSEC(1000));
	if (err == -ENOMSG) {
		LOG_ERR("No trigger event received");
		TEST_FAIL();
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	if (chan != &TRIGGER_CHAN) {
		LOG_ERR("Received message from wrong channel");
		TEST_FAIL();
	}
	TEST_ASSERT_EQUAL(expected_trig_type, trig_type);
}

static void check_no_trigger_events(uint32_t time_in_seconds)
{
	const struct zbus_channel *chan;
	enum trigger_type trig_type;
	int err;

	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_SECONDS(time_in_seconds));

	err = zbus_sub_wait_msg(&trigger_subscriber, &chan, &trig_type, K_MSEC(1000));
	if (err == 0) {
		LOG_ERR("Received trigger event with type %d", trig_type);
		TEST_FAIL();
	}
}

static void check_no_trigger_mode_events(uint32_t time_in_seconds)
{
	const struct zbus_channel *chan;
	enum trigger_mode trig_mode;
	int err;

	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_SECONDS(time_in_seconds));

	err = zbus_sub_wait_msg(&trig_mode_subscriber, &chan, &trig_mode, K_MSEC(1000));
	if (err == 0) {
		LOG_ERR("Received trigger mode event with type %d", trig_mode);
		TEST_FAIL();
	}
}

static void send_frequent_poll_duration_timer_expiry(void)
{
	/* Wait until CONFIG_FREQUENT_POLL_DURATION_INTERVAL_SEC */
	k_sleep(K_SECONDS(CONFIG_FREQUENT_POLL_DURATION_INTERVAL_SEC));

	uint32_t interval = ((CONFIG_FREQUENT_POLL_DURATION_INTERVAL_SEC /
			     FREQUENT_POLL_TRIGGER_INTERVAL_SEC) - 1);

	/* Check if the required number of trigger events was sent during frequent poll state.
	 * We reduce 1 from the expected number of trigger events because one trigger event is sent
	 * when entering the frequent poll state.
	 */

	check_trigger_event(TRIGGER_POLL);
	check_trigger_event(TRIGGER_FOTA_POLL);

	for (int i = 0; i < interval; i++) {
		check_trigger_event(TRIGGER_DATA_SAMPLE);
		check_trigger_event(TRIGGER_POLL);
		check_trigger_event(TRIGGER_FOTA_POLL);
		check_trigger_event(TRIGGER_POLL);
		check_trigger_event(TRIGGER_FOTA_POLL);
	}
}

static void go_to_frequent_poll_state(void)
{
	send_cloud_connected_ready_to_send();

	check_trigger_mode_event(TRIGGER_MODE_POLL);
	check_trigger_event(TRIGGER_DATA_SAMPLE);
	check_trigger_event(TRIGGER_POLL);
	check_trigger_event(TRIGGER_FOTA_POLL);
}

static void go_to_normal_state(void)
{
	go_to_frequent_poll_state();
	send_frequent_poll_duration_timer_expiry();
	check_trigger_mode_event(TRIGGER_MODE_NORMAL);
}

void test_init_to_frequent_poll(void)
{
	/* When */
	send_cloud_connected_ready_to_send();

	/* Then */
	check_trigger_mode_event(TRIGGER_MODE_POLL);
	check_trigger_event(TRIGGER_DATA_SAMPLE);
	check_trigger_event(TRIGGER_POLL);
	check_trigger_event(TRIGGER_FOTA_POLL);

	/* Cleanup */
	send_cloud_disconnected();
}

void test_frequent_poll_to_normal(void)
{
	/* Given */
	go_to_frequent_poll_state();

	/* When */
	send_frequent_poll_duration_timer_expiry();

	/* Then */
	check_trigger_mode_event(TRIGGER_MODE_NORMAL);
	check_no_trigger_events(CONFIG_APP_TRIGGER_TIMEOUT_SECONDS - 10);

	k_sleep(K_SECONDS(10));

	check_trigger_event(TRIGGER_DATA_SAMPLE);
	check_trigger_event(TRIGGER_POLL);
	check_trigger_event(TRIGGER_FOTA_POLL);

	/* Cleanup */
	send_cloud_disconnected();
}

void test_frequent_poll_to_blocked(void)
{
	/* Given */
	go_to_frequent_poll_state();

	/* When */
	send_location_event(LOCATION_SEARCH_STARTED);

	/* Then */
	check_no_trigger_events(FREQUENT_POLL_TRIGGER_INTERVAL_SEC * 2);

	/* Cleanup */
	send_cloud_disconnected();
}

void test_normal_to_blocked(void)
{
	/* Given */
	go_to_normal_state();

	/* When */
	send_location_event(LOCATION_SEARCH_STARTED);

	/* Then */
	check_no_trigger_events(CONFIG_APP_TRIGGER_TIMEOUT_SECONDS * 2);

	/* Cleanup */
	send_cloud_disconnected();
}

static void button_handler(uint32_t button_states, uint32_t has_changed)
{
	int err;
	uint8_t button_number = 1;

	if (has_changed & button_states & DK_BTN1_MSK) {
		LOG_DBG("Button 1 pressed!");

		err = zbus_chan_pub(&BUTTON_CHAN, &button_number, K_SECONDS(1));
		if (err) {
			LOG_ERR("zbus_chan_pub, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}
	}
}

void test_normal_mode_to_frequent_poll_due_to_button_press(void)
{
	/* Given */
	go_to_normal_state();

	/* When */
	button_handler(DK_BTN1_MSK, DK_BTN1_MSK);

	/* Then */
	check_trigger_mode_event(TRIGGER_MODE_POLL);
	check_trigger_event(TRIGGER_DATA_SAMPLE);
	check_trigger_event(TRIGGER_POLL);
	check_trigger_event(TRIGGER_FOTA_POLL);

	/* Cleanup */
	send_cloud_disconnected();
}

void test_normal_mode_to_frequent_poll_due_to_config_update(void)
{
	/* Given */
	go_to_normal_state();

	/* When */
	send_config();

	/* Then */
	check_trigger_mode_event(TRIGGER_MODE_POLL);
	check_trigger_event(TRIGGER_DATA_SAMPLE);
	check_trigger_event(TRIGGER_POLL);
	check_trigger_event(TRIGGER_FOTA_POLL);

	/* Cleanup */
	send_cloud_disconnected();
}

void test_frequent_poll_to_blocked_to_frequent_poll(void)
{
	/* Given */
	go_to_frequent_poll_state();
	send_location_event(LOCATION_SEARCH_STARTED);

	/* When */
	send_location_event(LOCATION_SEARCH_DONE);

	/* Then */
	k_sleep(K_SECONDS(FREQUENT_POLL_TRIGGER_INTERVAL_SEC));
	check_trigger_event(TRIGGER_POLL);
	check_trigger_event(TRIGGER_FOTA_POLL);
	check_trigger_event(TRIGGER_DATA_SAMPLE);
	check_no_trigger_mode_events(5);

	check_trigger_event(TRIGGER_POLL);
	check_trigger_event(TRIGGER_FOTA_POLL);

	/* Cleanup */
	send_cloud_disconnected();
}

void test_frequent_poll_to_blocked_to_normal(void)
{
	/* Given */
	go_to_frequent_poll_state();
	send_location_event(LOCATION_SEARCH_STARTED);

	/* When */
	k_sleep(K_SECONDS(CONFIG_FREQUENT_POLL_DURATION_INTERVAL_SEC));
	send_location_event(LOCATION_SEARCH_DONE);

	/* Then */
	check_trigger_mode_event(TRIGGER_MODE_NORMAL);
	check_no_trigger_events(CONFIG_APP_TRIGGER_TIMEOUT_SECONDS - 10);

	k_sleep(K_SECONDS(10));

	check_trigger_event(TRIGGER_DATA_SAMPLE);
	check_trigger_event(TRIGGER_POLL);
	check_trigger_event(TRIGGER_FOTA_POLL);

	check_no_trigger_events(CONFIG_APP_TRIGGER_TIMEOUT_SECONDS - 10);

	/* Cleanup */
	send_cloud_disconnected();
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
