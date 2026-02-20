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
#include <date_time.h>

#include "dk_buttons_and_leds.h"
#include "app_common.h"
#include "power.h"
#include "network.h"
#include "environmental.h"
#include "cloud.h"
#include "fota.h"
#include "location.h"
#include "led.h"
#include "button.h"
#include "storage.h"
#include "checks.h"
#include "cbor_helper.h"

DEFINE_FFF_GLOBALS;

#define HOUR_IN_SECONDS 3600
#define WEEK_IN_SECONDS HOUR_IN_SECONDS * 24 * 7

FAKE_VALUE_FUNC(int, dk_buttons_init, button_handler_t);
FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);
FAKE_VOID_FUNC(sys_reboot, int);

LOG_MODULE_REGISTER(main_module_test, 4);

/* Define the channels for testing */
ZBUS_CHAN_DEFINE(POWER_CHAN,
	struct power_msg,
	NULL,
	NULL,
	ZBUS_OBSERVERS_EMPTY,
	ZBUS_MSG_INIT(0)
);
ZBUS_CHAN_DEFINE(BUTTON_CHAN,
	struct button_msg,
	NULL,
	NULL,
	ZBUS_OBSERVERS_EMPTY,
	ZBUS_MSG_INIT(0)
);
ZBUS_CHAN_DEFINE(NETWORK_CHAN,
	struct network_msg,
	NULL,
	NULL,
	ZBUS_OBSERVERS_EMPTY,
	ZBUS_MSG_INIT(.type = NETWORK_DISCONNECTED)
);
ZBUS_CHAN_DEFINE(CLOUD_CHAN,
	struct cloud_msg,
	NULL,
	NULL,
	ZBUS_OBSERVERS_EMPTY,
	ZBUS_MSG_INIT(.type = CLOUD_DISCONNECTED)
);
ZBUS_CHAN_DEFINE(ENVIRONMENTAL_CHAN,
	struct environmental_msg,
	NULL,
	NULL,
	ZBUS_OBSERVERS_EMPTY,
	ZBUS_MSG_INIT(0)
);
ZBUS_CHAN_DEFINE(FOTA_CHAN,
	enum fota_msg_type,
	NULL,
	NULL,
	ZBUS_OBSERVERS_EMPTY,
	ZBUS_MSG_INIT(0)
);
ZBUS_CHAN_DEFINE(LOCATION_CHAN,
	struct location_msg,
	NULL,
	NULL,
	ZBUS_OBSERVERS_EMPTY,
	ZBUS_MSG_INIT(0)
);
ZBUS_CHAN_DEFINE(LED_CHAN,
	struct led_msg,
	NULL,
	NULL,
	ZBUS_OBSERVERS_EMPTY,
	ZBUS_MSG_INIT(0)
);
ZBUS_CHAN_DEFINE(STORAGE_CHAN,
	struct storage_msg,
	NULL,
	NULL,
	ZBUS_OBSERVERS_EMPTY,
	ZBUS_MSG_INIT(0)
);

/* Helper functions for sending messages */

static void send_cloud_connected(void)
{
	struct cloud_msg cloud_msg = {
		.type = CLOUD_CONNECTED,
	};

	int err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_cloud_disconnected(void)
{
	struct cloud_msg cloud_msg = {
		.type = CLOUD_DISCONNECTED,
	};

	int err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_network_disconnected(void)
{
	struct network_msg network_msg = {
		.type = NETWORK_DISCONNECTED,
	};

	int err = zbus_chan_pub(&NETWORK_CHAN, &network_msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_location_search_done(void)
{
	struct location_msg msg = {
		.type = LOCATION_SEARCH_DONE,
	};

	int err = zbus_chan_pub(&LOCATION_CHAN, &msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_button_press_short(void)
{
	struct button_msg button_msg = {
		.type = BUTTON_PRESS_SHORT,
		.button_number = 1
	};

	int err = zbus_chan_pub(&BUTTON_CHAN, &button_msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_button_press_long(void)
{
	struct button_msg button_msg = {
		.type = BUTTON_PRESS_LONG,
		.button_number = 1
	};

	int err = zbus_chan_pub(&BUTTON_CHAN, &button_msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_storage_threshold_reached(void)
{
	struct storage_msg storage_msg = {
		.type = STORAGE_THRESHOLD_REACHED,
	};
	int err = zbus_chan_pub(&STORAGE_CHAN, &storage_msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_storage_batch_close(void)
{
	struct storage_msg storage_msg = {
		.type = STORAGE_BATCH_CLOSE,
	};
	int err = zbus_chan_pub(&STORAGE_CHAN, &storage_msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_fota_msg(enum fota_msg_type msg)
{
	int err;

	LOG_INF("Sending FOTA message: %d", msg);

	err = zbus_chan_pub(&FOTA_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_MSEC(100));
}

static void config_change_sampling_interval(uint32_t sampling_interval)
{
	int err;
	struct cloud_msg msg = {
		.type = CLOUD_SHADOW_RESPONSE_DESIRED,
	};
	struct config_params config = {
		.sample_interval = sampling_interval,
	};
	size_t encoded_len = 0;

	err = encode_shadow_parameters_to_cbor(&config, 0, 0, msg.response.buffer,
							 sizeof(msg.response.buffer), &encoded_len);
	if (err != 0) {
		TEST_FAIL_MESSAGE("Failed to encode CBOR parameters");
	}

	msg.response.buffer_data_len = encoded_len;

	err = zbus_chan_pub(&CLOUD_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
}

static void config_change_cloud_update_interval(uint32_t update_interval)
{
	int err;
	struct cloud_msg msg = {
		.type = CLOUD_SHADOW_RESPONSE_DESIRED,
	};
	struct config_params config = {
		.update_interval = update_interval,
	};
	size_t encoded_len = 0;

	err = encode_shadow_parameters_to_cbor(&config, 0, 0, msg.response.buffer,
							 sizeof(msg.response.buffer), &encoded_len);
	if (err != 0) {
		TEST_FAIL_MESSAGE("Failed to encode CBOR parameters");
	}

	msg.response.buffer_data_len = encoded_len;

	err = zbus_chan_pub(&CLOUD_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
}

static void config_change_all(uint32_t update_interval, uint32_t sample_interval,
			      uint32_t storage_threshold)
{
	int err;
	struct cloud_msg msg = {
		.type = CLOUD_SHADOW_RESPONSE_DELTA,
	};
	struct config_params config = {
		.update_interval = update_interval,
		.sample_interval = sample_interval,
		.storage_threshold = storage_threshold,
		.storage_threshold_valid = true,
	};
	size_t encoded_len = 0;

	err = encode_shadow_parameters_to_cbor(&config, 0, 0, msg.response.buffer,
					       sizeof(msg.response.buffer), &encoded_len);
	if (err != 0) {
		TEST_FAIL_MESSAGE("Failed to encode CBOR parameters");
	}

	msg.response.buffer_data_len = encoded_len;

	err = zbus_chan_pub(&CLOUD_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
}

/* Restart the cloud update timer by doing a immediate cloud update using a long button press */
static void restart_cloud_timer(void)
{
	send_button_press_long();
	expect_storage_event(STORAGE_BATCH_REQUEST);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);

	send_storage_batch_close();
	expect_storage_event(STORAGE_BATCH_CLOSE);
}

/* Restart the sampling timer by doing a immediate sample using a short button press */
static void restart_sample_timer(void)
{
	send_button_press_short();
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
}

/* "Disable" the sampling timer by setting a very long sample interval through the shadow */
static void disable_sample_timer(void)
{
	config_change_sampling_interval(99999);
	expect_cloud_event(CLOUD_SHADOW_RESPONSE_DESIRED);
	expect_cloud_event(CLOUD_SHADOW_UPDATE_REPORTED);
}

void setUp(void)
{
	RESET_FAKE(dk_buttons_init);
	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(task_wdt_add);
	RESET_FAKE(sys_reboot);

	/* Ensure clean disconnected state */
	send_cloud_disconnected();
	k_sleep(K_MSEC(500));

	/* Trigger a button press to "burn" the current sampling cycle and reset state.
	 * This bypasses the "too soon to sample" protection and ensures each test
	 * starts with a known, clean state.
	 */
	send_button_press_short();
	send_location_search_done();
	/* Wait for sampling to complete and return to waiting state */
	k_sleep(K_MSEC(100));

	FFF_RESET_HISTORY();

	/* Clear any stale events from cleanup */
	purge_all_events();
}

/* Test functions */

void test_init_first_connection(void)
{
	/* Connect to cloud */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);

	/* First connection should trigger fota poll and get shadow desired */
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DESIRED);
}

void test_short_button_press_connected(void)
{
	/* Connect to cloud */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);

	/* Short button press should trigger sampling immediately */
	send_button_press_short();
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Complete location search */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
}

void test_long_button_press_connected(void)
{
	/* Connect to cloud */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);

	/* Long button press should trigger sending start and cloud poll */
	send_button_press_long();
	expect_storage_event(STORAGE_BATCH_REQUEST);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);
}

void test_threshold_reached_connected(void)
{
	/* Connect to cloud */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);

	/* Long button press should trigger sending start and cloud poll */
	send_storage_threshold_reached();
	expect_storage_event(STORAGE_THRESHOLD_REACHED);
	expect_storage_event(STORAGE_BATCH_REQUEST);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);
}

void test_short_button_press_disconnected(void)
{
	/* Short button press should trigger sampling immediately */
	send_button_press_short();
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Complete location search */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
}

void test_long_button_press_disconnected(void)
{
	/* Long button press when disconnected should be ignored */
	send_button_press_long();
	expect_no_events(500);
}

void test_threshold_reached_disconnected(void)
{
	/* Threshold reached when disconnected should be ignored */
	send_storage_threshold_reached();
	expect_storage_event(STORAGE_THRESHOLD_REACHED);
	expect_no_events(500);
}

void test_fota_downloading(void)
{
	/* Connect to cloud */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);

	/* Transition to STATE_FOTA_DOWNLOADING */
	send_fota_msg(FOTA_DOWNLOADING_UPDATE);
	expect_fota_event(FOTA_DOWNLOADING_UPDATE);

	/* A cloud ready message and button trigger should now cause no action */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);
	expect_no_events(7200);
	send_button_press_short();
	expect_no_events(7200);

	/* Cleanup */
	send_fota_msg(FOTA_DOWNLOAD_CANCELED);
	expect_fota_event(FOTA_DOWNLOAD_CANCELED);

	/* Should restore to previous state (connected waiting) and resume operation */
	expect_location_event(LOCATION_SEARCH_TRIGGER);
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
}

void test_fota_waiting_for_network_disconnect(void)
{
	/* Connect to cloud */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);

	/* Transition to STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT */
	send_fota_msg(FOTA_DOWNLOADING_UPDATE);
	expect_fota_event(FOTA_DOWNLOADING_UPDATE);
	send_fota_msg(FOTA_SUCCESS_REBOOT_NEEDED);
	expect_fota_event(FOTA_SUCCESS_REBOOT_NEEDED);

	/* Veriy that the module sends NETWORK_DISCONNECT */
	expect_network_event(NETWORK_DISCONNECT);

	expect_no_events(10);

	/* Send a NETWORK_DISCONNECTED event to trigger transition to STATE_FOTA_REBOOTING */
	send_network_disconnected();
	expect_network_event(NETWORK_DISCONNECTED);
	send_cloud_disconnected();
	expect_cloud_event(CLOUD_DISCONNECTED);

	/* Verify that the module sends STORAGE_CLEAR before fota reboot */
	expect_storage_event(STORAGE_CLEAR);

	/* Give the system time to reboot */
	k_sleep(K_SECONDS(10));

	TEST_ASSERT_EQUAL(1, sys_reboot_fake.call_count);

	/* Cleanup */
	send_fota_msg(FOTA_DOWNLOAD_CANCELED);
	expect_fota_event(FOTA_DOWNLOAD_CANCELED);
}

void test_fota_waiting_for_network_disconnect_to_apply_image(void)
{
	/* Connect to cloud */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);

	/* Transition to STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT_TO_APPLY_IMAGE */
	send_fota_msg(FOTA_DOWNLOADING_UPDATE);
	expect_fota_event(FOTA_DOWNLOADING_UPDATE);
	send_fota_msg(FOTA_IMAGE_APPLY_NEEDED);
	expect_fota_event(FOTA_IMAGE_APPLY_NEEDED);

	/* Veriy that the module sends NETWORK_DISCONNECT */
	expect_network_event(NETWORK_DISCONNECT);

	expect_no_events(10);

	/* Send a NETWORK_DISCONNECTED event to trigger transition to STATE_FOTA_APPLYING_IMAGE */
	send_network_disconnected();
	expect_network_event(NETWORK_DISCONNECTED);

	/* Verify that the module sends FOTA_IMAGE_APPLY */
	expect_fota_event(FOTA_IMAGE_APPLY);

	/* Trigger reboot */
	send_fota_msg(FOTA_SUCCESS_REBOOT_NEEDED);
	expect_fota_event(FOTA_SUCCESS_REBOOT_NEEDED);

	/* Verify that the module sends STORAGE_CLEAR before fota reboot */
	expect_storage_event(STORAGE_CLEAR);

	/* Give the system time to reboot */
	k_sleep(K_SECONDS(10));

	TEST_ASSERT_EQUAL(1, sys_reboot_fake.call_count);

	/* Cleanup */
	send_fota_msg(FOTA_DOWNLOAD_CANCELED);
	expect_fota_event(FOTA_DOWNLOAD_CANCELED);
}

void test_sensor_timer_multiple_expiries(void)
{
	/* Connect to cloud */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);

	/* Dont want cloud timer to interfere */
	restart_cloud_timer();
	restart_sample_timer();

	/* Wait for sample timer to trigger sampling */
	k_sleep(K_SECONDS(CONFIG_APP_SAMPLING_INTERVAL_SECONDS));
	expect_timer_event(TIMER_EXPIRED_SAMPLE_DATA);
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Complete location search */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);

	/* Wait for next sample timer */
	k_sleep(K_SECONDS(CONFIG_APP_SAMPLING_INTERVAL_SECONDS));
	expect_timer_event(TIMER_EXPIRED_SAMPLE_DATA);
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Complete location search */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
}

// TODO: This needs cleaning up
void test_cloud_timer_multiple_expiries(void)
{
	/* Connect to cloud */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);

	disable_sample_timer();
	expect_timer_event(TIMER_CONFIG_CHANGED);

	restart_cloud_timer();

	for (int i = 0; i < 5; i++) {
		/* Wait for cloud timer to trigger */
		k_sleep(K_SECONDS(CONFIG_APP_CLOUD_UPDATE_INTERVAL_SECONDS));
		expect_timer_event(TIMER_EXPIRED_CLOUD);
		expect_storage_event(STORAGE_BATCH_REQUEST);
		expect_fota_event(FOTA_POLL_REQUEST);
		expect_cloud_event(CLOUD_SHADOW_GET_DELTA);

		/* Close batch to signal send completion */
		send_storage_batch_close();
		expect_storage_event(STORAGE_BATCH_CLOSE);
	}
}

void test_sampling_during_cloud_send(void)
{
	/* Connect to cloud */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);

	/* Trigger cloud send */
	send_button_press_long();
	expect_storage_event(STORAGE_BATCH_REQUEST);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);

	send_button_press_short();
	expect_location_event(LOCATION_SEARCH_TRIGGER);
	/* Complete location search */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);

	expect_storage_event(STORAGE_BATCH_REQUEST);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);

	/* Close batch to signal send completion */
	send_storage_batch_close();
	expect_storage_event(STORAGE_BATCH_CLOSE);
}

void test_config_change(void)
{
	/* Connect to cloud */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);

	/* Restart timers to know the exact timing for the next triggers */
	restart_cloud_timer();
	restart_sample_timer();

	/* Change sample interval and verify that the new interval is respected */
	config_change_sampling_interval(300);
	expect_cloud_event(CLOUD_SHADOW_RESPONSE_DESIRED);
	expect_cloud_event(CLOUD_SHADOW_UPDATE_REPORTED);
	expect_timer_event(TIMER_CONFIG_CHANGED);

	k_sleep(K_SECONDS(300));
	expect_timer_event(TIMER_EXPIRED_SAMPLE_DATA);
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Complete location search */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);

	/* Disable sample timer to avoid interference */
	disable_sample_timer();
	expect_timer_event(TIMER_CONFIG_CHANGED);

	/* Restart cloud timer to know the exact timing for the next triggers */
	restart_cloud_timer();

	/* Change cloud update interval and verify that the new interval is respected */
	config_change_cloud_update_interval(1000);
	expect_cloud_event(CLOUD_SHADOW_RESPONSE_DESIRED);
	expect_cloud_event(CLOUD_SHADOW_UPDATE_REPORTED);
	expect_timer_event(TIMER_CONFIG_CHANGED);

	k_sleep(K_SECONDS(1000));
	expect_timer_event(TIMER_EXPIRED_CLOUD);
	expect_storage_event(STORAGE_BATCH_REQUEST);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);

	/* Close batch to signal send completion */
	send_storage_batch_close();
	expect_storage_event(STORAGE_BATCH_CLOSE);

	/* Close this batch too */
	send_storage_batch_close();
	expect_storage_event(STORAGE_BATCH_CLOSE);

	/* Restart timers to know the exact timing for the next triggers */
	restart_cloud_timer();
	restart_sample_timer();

	/* Change all parameters at once and verify that all changes are respected */
	config_change_all(500, 300, 3);
	expect_storage_event(STORAGE_SET_THRESHOLD);
	expect_cloud_event(CLOUD_SHADOW_RESPONSE_DELTA);
	expect_cloud_event(CLOUD_SHADOW_UPDATE_REPORTED);
	expect_timer_event(TIMER_CONFIG_CHANGED);

	k_sleep(K_SECONDS(300));
	expect_timer_event(TIMER_EXPIRED_SAMPLE_DATA);
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Complete location search */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);

	k_sleep(K_SECONDS(200));
	expect_timer_event(TIMER_EXPIRED_CLOUD);
	expect_storage_event(STORAGE_BATCH_REQUEST);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);

	/* Close batch to signal send completion */
	send_storage_batch_close();
	expect_storage_event(STORAGE_BATCH_CLOSE);
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
