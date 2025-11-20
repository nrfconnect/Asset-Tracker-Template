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

DEFINE_FFF_GLOBALS;

#define HOUR_IN_SECONDS	3600
#define WEEK_IN_SECONDS HOUR_IN_SECONDS * 24 * 7

FAKE_VALUE_FUNC(int, dk_buttons_init, button_handler_t);
FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);
FAKE_VOID_FUNC(date_time_register_handler, date_time_evt_handler_t);
FAKE_VOID_FUNC(sys_reboot, int);

LOG_MODULE_REGISTER(main_module_test, 1);

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

void setUp(void)
{
	RESET_FAKE(dk_buttons_init);
	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(task_wdt_add);
	RESET_FAKE(date_time_register_handler);
	RESET_FAKE(sys_reboot);

	FFF_RESET_HISTORY();

	purge_all_events();
}

static void button_handler(uint32_t button_states, uint32_t has_changed)
{
	int err;
	struct button_msg button_msg = {
		.type = BUTTON_PRESS_LONG,
		.button_number = 1
	};

	if (has_changed & button_states & DK_BTN1_MSK) {
		LOG_DBG("Button 1 pressed!");

		err = zbus_chan_pub(&BUTTON_CHAN, &button_msg, K_SECONDS(1));
		if (err) {
			LOG_ERR("zbus_chan_pub, error: %d", err);
			TEST_FAIL();
			return;
		}
	}
}

static void send_cloud_connected(void)
{
	struct cloud_msg cloud_msg = {
		.type = CLOUD_CONNECTED,
	};

	int err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_SECONDS(1));

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

static void send_fota_msg(enum fota_msg_type msg)
{
	int err;

	err = zbus_chan_pub(&FOTA_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_MSEC(100));
}

static void twelve_hour_interval_set(void)
{
	int err;
	const struct cloud_msg msg = {
		.type = CLOUD_SHADOW_RESPONSE_DESIRED,
		/* JSON equivalent string: "{"config":{"update_interval": 43200 }}" */
		.response.buffer = {
		0xA1,  /* Map of 1 key-value pair */
		0x66,  /* Text string of length 6 */
		0x63, 0x6F, 0x6E, 0x66, 0x69, 0x67,  /* 'c', 'o', 'n', 'f', 'i', 'g' => "config" */
		0xA1,  /* Nested map with 1 key-value pair */
		0x6F,  /* Text string of length 15 */
		0x75, 0x70, 0x64, 0x61, 0x74, 0x65, /* 'u', 'p', 'd', 'a', 't', 'e' => "update" */
		0x5F, 0x69, 0x6E, 0x74, 0x65, 0x72, /* '_', 'i', 'n', 't', 'e', 'r' => "_inter" */
		0x76, 0x61, 0x6C, /* 'v', 'a', 'l' => "val" */
		0x19, 0xA8, 0xC0  /* Unsigned integer (uint16) with value 43200 */
		},
		.response.buffer_data_len = 28,
	};

	err = zbus_chan_pub(&CLOUD_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
}

static void config_change_to_buffer_mode(void)
{
	int err;
	const struct cloud_msg msg = {
		.type = CLOUD_SHADOW_RESPONSE_DELTA,
		/* JSON equivalent string: "{"config":{"update_interval": 300, "sample_interval": 60, "buffer_mode": true}}" */
		.response.buffer = {
		0xA1,  /* Map of 1 key-value pair */
		0x66,  /* Text string of length 6 */
		0x63, 0x6F, 0x6E, 0x66, 0x69, 0x67,  /* 'c', 'o', 'n', 'f', 'i', 'g' => "config" */
		0xA3,  /* Nested map with 3 key-value pairs */
		0x6F,  /* Text string of length 15 */
		0x75, 0x70, 0x64, 0x61, 0x74, 0x65, /* 'u', 'p', 'd', 'a', 't', 'e' => "update" */
		0x5F, 0x69, 0x6E, 0x74, 0x65, 0x72, /* '_', 'i', 'n', 't', 'e', 'r' => "_inter" */
		0x76, 0x61, 0x6C, /* 'v', 'a', 'l' => "val" */
		0x1A, 0x00, 0x00, 0x01, 0x2C,  /* Unsigned integer (uint32) with value 300 */
		0x6F,  /* Text string of length 15 */
		0x73, 0x61, 0x6D, 0x70, 0x6C, 0x65, /* 's', 'a', 'm', 'p', 'l', 'e' => "sample" */
		0x5F, 0x69, 0x6E, 0x74, 0x65, 0x72, /* '_', 'i', 'n', 't', 'e', 'r' => "_inter" */
		0x76, 0x61, 0x6C, /* 'v', 'a', 'l' => "val" */
		0x1A, 0x00, 0x00, 0x00, 0x3C,  /* Unsigned integer (uint32) with value 60 */
		0x6B,  /* Text string of length 11 */
		0x62, 0x75, 0x66, 0x66, 0x65, 0x72, /* 'b', 'u', 'f', 'f', 'e', 'r' => "buffer" */
		0x5F, 0x6D, 0x6F, 0x64, 0x65, /* '_', 'm', 'o', 'd', 'e' => "_mode" */
		0xF5  /* Boolean true */
		},
		.response.buffer_data_len = 64,
	};

	err = zbus_chan_pub(&CLOUD_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
}

static void config_change_invalid_update_interval_zero(void)
{
	int err;
	const struct cloud_msg msg = {
		.type = CLOUD_SHADOW_RESPONSE_DELTA,
		/* JSON equivalent string: "{"config":{"update_interval": 0 }}" */
		.response.buffer = {
		0xA1,  /* Map of 1 key-value pair */
		0x66,  /* Text string of length 6 */
		0x63, 0x6F, 0x6E, 0x66, 0x69, 0x67,  /* 'c', 'o', 'n', 'f', 'i', 'g' => "config" */
		0xA1,  /* Nested map with 1 key-value pair */
		0x6F,  /* Text string of length 15 */
		0x75, 0x70, 0x64, 0x61, 0x74, 0x65, /* 'u', 'p', 'd', 'a', 't', 'e' => "update" */
		0x5F, 0x69, 0x6E, 0x74, 0x65, 0x72, /* '_', 'i', 'n', 't', 'e', 'r' => "_inter" */
		0x76, 0x61, 0x6C, /* 'v', 'a', 'l' => "val" */
		0x1A, 0x00, 0x00, 0x00, 0x00  /* Unsigned integer (uint32) with value 0 */
		},
		.response.buffer_data_len = 30,
	};

	err = zbus_chan_pub(&CLOUD_CHAN, &msg, K_SECONDS(1));
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
static void send_storage_mode_request(enum storage_msg_type mode_type)
{
	struct storage_msg storage_msg = {
		.type = mode_type,
	};
	int err = zbus_chan_pub(&STORAGE_CHAN, &storage_msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_button_press(enum button_msg_type button_type)
{
	struct button_msg button_msg = {
		.type = button_type,
		.button_number = 1
	};
	int err = zbus_chan_pub(&BUTTON_CHAN, &button_msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

void test_init_state_passthrough_mode(void)
{
	expect_no_events(7200);
}

void test_init_to_sample_data_state(void)
{
	/* Give the app_main thread time to start and initialize */
	k_sleep(K_MSEC(500));

	/* The module starts in passthrough mode by default in test config */
	/* Connect to cloud to trigger sampling */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);
	expect_cloud_event(CLOUD_SHADOW_GET_DESIRED);

	/* Should immediately trigger location search in passthrough mode */
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Complete location search */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);

	k_sleep(K_SECONDS(1));

	/* In passthrough mode, sensor triggers and cloud polling happen immediately */
	expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);

	/* Cleanup */
	send_cloud_disconnected();
	expect_cloud_event(CLOUD_DISCONNECTED);
	expect_no_events(7200);
}

void test_button_press_on_connected(void)
{
	/* Connect to cloud first */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);

	/* Initial transition to STATE_SAMPLE_DATA */
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Transition to STATE_WAIT_FOR_TRIGGER */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);

	/* Wait for 10 seconds before triggering a button press */
	k_sleep(K_SECONDS(10));

	/* Long button press should trigger poll and data send */
	button_handler(DK_BTN1_MSK, DK_BTN1_MSK);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);

	/* Cleanup */
	send_cloud_disconnected();
	expect_cloud_event(CLOUD_DISCONNECTED);
	expect_no_events(7200);
}

void test_button_press_on_disconnected(void)
{
	/* Given */
	send_cloud_disconnected();
	expect_cloud_event(CLOUD_DISCONNECTED);

	/* When */
	button_handler(DK_BTN1_MSK, DK_BTN1_MSK);
	k_sleep(K_SECONDS(5));

	/* Then */
	expect_no_events(7200);
}

void test_fota_downloading(void)
{
	/* Ensure cloud is connected */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);

	/* A location search trigger should be sent */
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Transition to STATE_FOTA_DOWNLOADING */
	send_fota_msg(FOTA_DOWNLOADING_UPDATE);
	expect_fota_event(FOTA_DOWNLOADING_UPDATE);

	k_sleep(K_SECONDS(1));

	/* A cloud ready message and button trigger should now cause no action */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);
	expect_no_events(7200);
	button_handler(DK_BTN1_MSK, DK_BTN1_MSK);
	expect_no_events(7200);

	/* Cleanup */
	send_fota_msg(FOTA_DOWNLOAD_CANCELED);
	expect_fota_event(FOTA_DOWNLOAD_CANCELED);

	expect_location_event(LOCATION_SEARCH_TRIGGER);

	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);

	/* Cleanup */
	send_cloud_disconnected();
	expect_cloud_event(CLOUD_DISCONNECTED);
}

void test_fota_waiting_for_network_disconnect(void)
{
	/* Ensure cloud is connected */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);

	/* A location search trigger should be sent */
	expect_location_event(LOCATION_SEARCH_TRIGGER);

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

	/* Give the system time to reboot */
	k_sleep(K_SECONDS(10));

	TEST_ASSERT_EQUAL(1, sys_reboot_fake.call_count);

	/* Cleanup */
	send_fota_msg(FOTA_DOWNLOAD_CANCELED);
	expect_fota_event(FOTA_DOWNLOAD_CANCELED);

	expect_no_events(1);
}

void test_fota_waiting_for_network_disconnect_to_apply_image(void)
{
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

	/* Give the system time to reboot */
	k_sleep(K_SECONDS(10));

	TEST_ASSERT_EQUAL(1, sys_reboot_fake.call_count);

	/* Cleanup */
	send_fota_msg(FOTA_DOWNLOAD_CANCELED);
	expect_fota_event(FOTA_DOWNLOAD_CANCELED);

	expect_no_events(1);
}

/* Passthrough Mode Tests */
void test_passthrough_mode_initialization(void)
{
	/* Give the app_main thread time to start and initialize */
	k_sleep(K_MSEC(500));

	/* App starts in passthrough mode by default in test config */
	/* Connect to cloud to trigger sampling */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);

	/* Should immediately trigger location search in passthrough mode */
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Complete location search */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);

	/* In passthrough mode, sensor triggers and cloud polling happen immediately */
	expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);

	/* Cleanup */
	send_cloud_disconnected();
	expect_cloud_event(CLOUD_DISCONNECTED);
	expect_no_events(1);
}

void test_passthrough_sampling_and_immediate_send(void)
{
	/* App starts in passthrough mode by default */
	/* Connect to cloud */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Complete sampling */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);

	/* Verify immediate sensor sampling and cloud polling */
	expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);

	/* Wait for next sample interval and verify automatic triggering */
	k_sleep(K_SECONDS(CONFIG_APP_CLOUD_SYNC_INTERVAL_SECONDS));
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Cleanup */
	send_cloud_disconnected();
	expect_cloud_event(CLOUD_DISCONNECTED);
	expect_no_events(1);
}

void test_passthrough_disconnected_behavior(void)
{
	/* App starts in passthrough mode, ensure we're disconnected */
	send_cloud_disconnected();
	expect_cloud_event(CLOUD_DISCONNECTED);

	/* In passthrough disconnected mode, no sampling should occur */
	k_sleep(K_SECONDS(5));
	expect_no_events(1);

	/* Button presses should be ignored when disconnected */
	send_button_press(BUTTON_PRESS_SHORT);
	k_sleep(K_SECONDS(2));
	expect_no_events(1);

	/* Reconnect should trigger immediate sampling */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Cleanup */
	send_cloud_disconnected();
	expect_cloud_event(CLOUD_DISCONNECTED);
	expect_no_events(1);
}

void test_passthrough_button_interactions(void)
{
	/* App starts in passthrough mode, connect */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Complete initial sampling */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);

	/* Short button press should trigger immediate sampling */
	send_button_press(BUTTON_PRESS_SHORT);
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Complete sampling */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);

	/* Long button press should trigger immediate cloud poll */
	send_button_press(BUTTON_PRESS_LONG);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);

	/* Cleanup */
	send_cloud_disconnected();
	expect_cloud_event(CLOUD_DISCONNECTED);
	expect_no_events(1);
}

void test_passthrough_timer_cancellation_on_disconnect(void)
{
	/* App starts in passthrough mode, connect to start sampling cycle */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Complete initial sampling to enter waiting state with active timer */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);

	/* Now in waiting state with sampling timer active.
	 * Disconnect from cloud - this should cancel the timer.
	 */
	send_cloud_disconnected();
	expect_cloud_event(CLOUD_DISCONNECTED);

	/* Wait longer than the normal sampling interval to verify timer was cancelled.
	 * If timer wasn't cancelled, we would see a LOCATION_SEARCH_TRIGGER.
	 */
	k_sleep(K_SECONDS(CONFIG_APP_CLOUD_SYNC_INTERVAL_SECONDS + 10));
	expect_no_events(1);

	/* Reconnect should trigger immediate sampling (not timer-based) */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Complete sampling and verify normal operation resumes */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);

	/* Test disconnect again during waiting state to ensure consistent behavior */
	send_cloud_disconnected();
	expect_cloud_event(CLOUD_DISCONNECTED);

	/* Again, verify no timer-based events occur after disconnect */
	k_sleep(K_SECONDS(CONFIG_APP_CLOUD_SYNC_INTERVAL_SECONDS + 5));
	expect_no_events(1);

	/* Final cleanup */
	expect_no_events(1);
}

/* Storage Mode Switching Tests */
void test_storage_mode_request_handling(void)
{
	int err;
	struct storage_msg storage_msg = {
		.type = STORAGE_MODE_BUFFER,
	};

	/* Test that main module can handle storage mode requests */
	send_storage_mode_request(STORAGE_MODE_BUFFER_REQUEST);
	expect_storage_event(STORAGE_MODE_BUFFER_REQUEST);

	/* Simulate storage module confirming the mode change */
	err = zbus_chan_pub(&STORAGE_CHAN, &storage_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	expect_storage_event(STORAGE_MODE_BUFFER);

	/* Give time for state transition */
	k_sleep(K_MSEC(500));

	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Now test normal operation in buffer mode */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);

	/* Complete sampling */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);

	/* Put the module back into passthrough mode */
	send_storage_mode_request(STORAGE_MODE_PASSTHROUGH_REQUEST);
	expect_storage_event(STORAGE_MODE_PASSTHROUGH_REQUEST);

	/* Simulate storage module confirming the mode change to passthrough */
	storage_msg.type = STORAGE_MODE_PASSTHROUGH;

	err = zbus_chan_pub(&STORAGE_CHAN, &storage_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	expect_storage_event(STORAGE_MODE_PASSTHROUGH);

	/* When switching to passthrough mode while connected, sampling begins immediately */
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Cleanup */
	send_cloud_disconnected();
	expect_cloud_event(CLOUD_DISCONNECTED);
	expect_no_events(1);
}

void test_cloud_timer_in_buffer_mode(void)
{
	int err;
	struct storage_msg storage_msg = {
		.type = STORAGE_MODE_BUFFER,
	};

	/* Switch to buffer mode first */
	err = zbus_chan_pub(&STORAGE_CHAN, &storage_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	expect_storage_event(STORAGE_MODE_BUFFER);

	/* Expect a location search trigger when entering buffer mode */
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Connect and complete initial sampling */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);

	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);

	/* After CONFIG_APP_CLOUD_SYNC_INTERVAL_SECONDS, the module should trigger a
	 * storage batch request, cloud polling and FOTA poll.
	 */
	k_sleep(K_SECONDS(CONFIG_APP_CLOUD_SYNC_INTERVAL_SECONDS));

	expect_storage_event(STORAGE_BATCH_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);
	expect_fota_event(FOTA_POLL_REQUEST);

	/* Cleanup */
	send_cloud_disconnected();
	expect_cloud_event(CLOUD_DISCONNECTED);
}

void test_timer_cancellation_during_fota(void)
{
	int err;
	struct storage_msg storage_msg = {
		.type = STORAGE_MODE_BUFFER,
	};

	/* Switch to buffer mode first */
	err = zbus_chan_pub(&STORAGE_CHAN, &storage_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	expect_storage_event(STORAGE_MODE_BUFFER);

	/* Start normal operation */
	send_cloud_connected();

	/* We would have received cloud and location events, but they should be ignored in this
	 * test.
	 */
	purge_cloud_events();
	purge_location_events();

	/* Trigger FOTA */
	send_fota_msg(FOTA_DOWNLOADING_UPDATE);
	expect_fota_event(FOTA_DOWNLOADING_UPDATE);

	/* During FOTA, no timer-based events should occur - verify by waiting multiple
	 * intervals.
	 */
	expect_no_events(CONFIG_APP_CLOUD_SYNC_INTERVAL_SECONDS * 5);

	/* Cancel FOTA and return to normal operation */
	send_fota_msg(FOTA_DOWNLOAD_CANCELED);
	expect_fota_event(FOTA_DOWNLOAD_CANCELED);

	/* Should resume normal timer-based operation */
	k_sleep(K_SECONDS(CONFIG_APP_CLOUD_SYNC_INTERVAL_SECONDS));
	expect_storage_event(STORAGE_BATCH_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);
	expect_fota_event(FOTA_POLL_REQUEST);

	/* Cleanup */
	send_cloud_disconnected();
}

void test_multiple_cloud_data_send_intervals(void)
{
	/* Connect and complete initial sampling */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);

	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);

	/* After CONFIG_APP_CLOUD_SYNC_INTERVAL_SECONDS, the module should trigger a
	 * storage batch request, cloud polling and FOTA poll.
	 */
	 k_sleep(K_SECONDS(CONFIG_APP_CLOUD_SYNC_INTERVAL_SECONDS));
	 expect_storage_event(STORAGE_BATCH_REQUEST);
	 expect_cloud_event(CLOUD_SHADOW_GET_DELTA);
	 expect_fota_event(FOTA_POLL_REQUEST);

	/* Test multiple update intervals to verify timer restarts correctly */
	for (int i = 0; i < 3; i++) {
		/* Wait for cloud timer to expire */
		k_sleep(K_SECONDS(CONFIG_APP_CLOUD_SYNC_INTERVAL_SECONDS + 10));

		/* Should trigger storage batch request and cloud polling */
		expect_storage_event(STORAGE_BATCH_REQUEST);
		expect_cloud_event(CLOUD_SHADOW_GET_DELTA);
		expect_fota_event(FOTA_POLL_REQUEST);

		/* Longer delay between iterations to avoid queue overflow */
		k_sleep(K_MSEC(500));
	}

	/* Cleanup */
	send_cloud_disconnected();
	expect_cloud_event(CLOUD_DISCONNECTED);
}

void test_cloud_data_send_with_sampling_interleaved(void)
{
	/* Connect and complete initial sampling */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);

	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);

	/* After CONFIG_APP_CLOUD_SYNC_INTERVAL_SECONDS, the module should trigger a
	 * storage batch request, cloud polling and FOTA poll.
	 */
	 k_sleep(K_SECONDS(CONFIG_APP_CLOUD_SYNC_INTERVAL_SECONDS));
	 expect_storage_event(STORAGE_BATCH_REQUEST);
	 expect_cloud_event(CLOUD_SHADOW_GET_DELTA);
	 expect_fota_event(FOTA_POLL_REQUEST);

	/* Test interleaved sampling and cloud data sending.
	 * To trigger immediate sending of data, we use a long button press.
	 */
	for (int i = 0; i < 10; i++) {
		/* Wait for sampling timer */
		k_sleep(K_SECONDS(CONFIG_APP_CLOUD_SYNC_INTERVAL_SECONDS));
		expect_location_event(LOCATION_SEARCH_TRIGGER);

		/* Complete sampling */
		send_location_search_done();
		expect_location_event(LOCATION_SEARCH_DONE);
		expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
		expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);

		send_button_press(BUTTON_PRESS_LONG);
		expect_storage_event(STORAGE_BATCH_REQUEST);
		expect_cloud_event(CLOUD_SHADOW_GET_DELTA);
		expect_fota_event(FOTA_POLL_REQUEST);
		expect_cloud_event(CLOUD_SHADOW_GET_DELTA);
	}

	/* Cleanup */
	send_cloud_disconnected();
	expect_cloud_event(CLOUD_DISCONNECTED);
}

void test_trigger_interval_change_in_connected(void)
{
	int err;
	struct storage_msg storage_msg = {
		.type = STORAGE_MODE_PASSTHROUGH
	};

	/* Ensure passthrough mode */
	err = zbus_chan_pub(&STORAGE_CHAN, &storage_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	expect_storage_event(STORAGE_MODE_PASSTHROUGH);

	/* Connect to cloud */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);

	/* Initial transition to passthrough mode triggers a sample */
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);

	/* As response to the shadow poll, the interval is set to 12 hours and passthrough
	 * is enabled.
	 */
	twelve_hour_interval_set();
	expect_cloud_event(CLOUD_SHADOW_RESPONSE_DESIRED);
	expect_cloud_event(CLOUD_SHADOW_UPDATE_REPORTED);
	expect_storage_event(STORAGE_MODE_PASSTHROUGH_REQUEST);

	/* Wait for the interval to almost expire and ensure no events are triggered in
	 * that time. Repeat this 10 times.
	 */
	for (int i = 0; i < 10; i++) {
		int elapsed_time;

		elapsed_time = wait_for_location_event(LOCATION_SEARCH_TRIGGER,
						       HOUR_IN_SECONDS * 12 + 1);
		TEST_ASSERT_INT_WITHIN(1, HOUR_IN_SECONDS * 12, elapsed_time);

		send_location_search_done();
		expect_location_event(LOCATION_SEARCH_DONE);
		expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
		expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
		expect_fota_event(FOTA_POLL_REQUEST);
		expect_cloud_event(CLOUD_SHADOW_GET_DELTA);
	}

	/* Cleanup */
	send_cloud_disconnected();
	expect_cloud_event(CLOUD_DISCONNECTED);
	expect_no_events(WEEK_IN_SECONDS);
}

void test_trigger_disconnect_and_connect_when_sampling(void)
{
	bool first_trigger_after_connect = true;

	/* Connect to cloud */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);

	/* As response to the shadow poll, the interval is set to 12 hours. */
	twelve_hour_interval_set();
	expect_cloud_event(CLOUD_SHADOW_RESPONSE_DESIRED);
	expect_cloud_event(CLOUD_SHADOW_UPDATE_REPORTED);
	expect_storage_event(STORAGE_MODE_PASSTHROUGH_REQUEST);

	/* Wait for the interval to almost expire and ensure no events are triggered in
	 * that time. Repeat this 10 times. Every second iteration, disconnect and connect.
	 */
	for (int i = 0; i < 10; i++) {
		int elapsed_time;
		uint32_t timeout = first_trigger_after_connect ? 1 : HOUR_IN_SECONDS * 12;

		elapsed_time = wait_for_location_event(LOCATION_SEARCH_TRIGGER,
						       HOUR_IN_SECONDS * 12);
		TEST_ASSERT_INT_WITHIN(1, timeout, elapsed_time);

		send_location_search_done();
		expect_location_event(LOCATION_SEARCH_DONE);
		expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
		expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
		expect_fota_event(FOTA_POLL_REQUEST);
		expect_cloud_event(CLOUD_SHADOW_GET_DELTA);

		first_trigger_after_connect = false;

		/* Disconnect and connect every second iteration */
		if (i % 2 == 0) {
			send_cloud_disconnected();
			expect_cloud_event(CLOUD_DISCONNECTED);
			expect_no_events(7200);
			send_cloud_connected();
			expect_cloud_event(CLOUD_CONNECTED);

			first_trigger_after_connect = true;
		}
	}

	/* Cleanup */
	send_cloud_disconnected();
	expect_cloud_event(CLOUD_DISCONNECTED);
	expect_no_events(WEEK_IN_SECONDS);
}

void test_config_change_all_parameters_passthrough_to_buffer(void)
{
	int err;
	struct storage_msg storage_msg;

	/* Start in passthrough mode (already tested in other tests) */
	storage_msg.type = STORAGE_MODE_PASSTHROUGH;
	err = zbus_chan_pub(&STORAGE_CHAN, &storage_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
	expect_storage_event(STORAGE_MODE_PASSTHROUGH);

	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);

	/* Complete initial passthrough sampling */
	expect_location_event(LOCATION_SEARCH_TRIGGER);
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);

	/* Send configuration change with all three parameters:
	 * update_interval: 300s, sample_interval: 60s, buffer_mode: true
	 */
	config_change_to_buffer_mode();
	expect_cloud_event(CLOUD_SHADOW_RESPONSE_DELTA);
	expect_cloud_event(CLOUD_SHADOW_UPDATE_REPORTED);

	/* Verify application requests storage mode change */
	expect_storage_event(STORAGE_MODE_BUFFER_REQUEST);

	/* Confirm the mode change in storage module */
	storage_msg.type = STORAGE_MODE_BUFFER;
	err = zbus_chan_pub(&STORAGE_CHAN, &storage_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
	expect_storage_event(STORAGE_MODE_BUFFER);

	/* Verify immediate sampling after mode switch (sample_start_time reset) */
	expect_location_event(LOCATION_SEARCH_TRIGGER);
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);

	/* Test interleaved sampling and cloud updates with new intervals
	 * Sample interval: 60s, Update interval: 300s (5 samples per update)
	 */
	for (int i = 0; i < 3; i++) {
		/* 4 sampling cycles at 60s each */
		for (int j = 0; j < 4; j++) {
			k_sleep(K_SECONDS(60));
			expect_location_event(LOCATION_SEARCH_TRIGGER);
			send_location_search_done();
			expect_location_event(LOCATION_SEARCH_DONE);
			expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
			expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
		}

		/* 5th sampling cycle (completes 300s) + cloud update */
		k_sleep(K_SECONDS(60));
		expect_location_event(LOCATION_SEARCH_TRIGGER);
		send_location_search_done();
		expect_location_event(LOCATION_SEARCH_DONE);
		expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
		expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);

		/* Cloud update at 300s interval */
		expect_storage_event(STORAGE_BATCH_REQUEST);
		expect_fota_event(FOTA_POLL_REQUEST);
		expect_cloud_event(CLOUD_SHADOW_GET_DELTA);
	}

	/* Cleanup */
	send_cloud_disconnected();
	expect_cloud_event(CLOUD_DISCONNECTED);
	expect_no_events(1);
}

void test_config_not_accepted_should_remain_in_current_mode(void)
{
	int err;
	struct storage_msg storage_msg;

	/* Start in passthrough mode (already tested in other tests) */
	storage_msg.type = STORAGE_MODE_PASSTHROUGH;
	err = zbus_chan_pub(&STORAGE_CHAN, &storage_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
	expect_storage_event(STORAGE_MODE_PASSTHROUGH);

	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);

	/* Complete initial passthrough sampling */
	expect_location_event(LOCATION_SEARCH_TRIGGER);
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);

	/* Send configuration change with invalid parameter (update_interval = 0)
	 * This should be rejected by the application.
	 */
	config_change_invalid_update_interval_zero();
	expect_cloud_event(CLOUD_SHADOW_RESPONSE_DELTA);

	/* Even though the configuration is invalid, we report our configuration to cloud. */
	expect_cloud_event(CLOUD_SHADOW_UPDATE_REPORTED);

	/* Verify normal passthrough operation continues with default interval */
	k_sleep(K_SECONDS(CONFIG_APP_CLOUD_SYNC_INTERVAL_SECONDS));
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Cleanup */
	send_cloud_disconnected();
	expect_cloud_event(CLOUD_DISCONNECTED);
	expect_no_events(7200);
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
