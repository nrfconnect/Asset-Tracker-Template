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
#include "checks.h"

/* Define the channels for testing */
ZBUS_CHAN_DEFINE(POWER_CHAN,
		 struct power_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);
ZBUS_CHAN_DEFINE(BUTTON_CHAN,
		 uint8_t,
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
		 enum location_msg_type,
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


DEFINE_FFF_GLOBALS;

#define HOUR_IN_SECONDS	3600
#define WEEK_IN_SECONDS HOUR_IN_SECONDS * 24 * 7

FAKE_VALUE_FUNC(int, dk_buttons_init, button_handler_t);
FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);
FAKE_VOID_FUNC(date_time_register_handler, date_time_evt_handler_t);
FAKE_VOID_FUNC(sys_reboot, int);

LOG_MODULE_REGISTER(trigger_module_test, 4);

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
	enum location_msg_type msg = LOCATION_SEARCH_DONE;

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
		.type = CLOUD_SHADOW_RESPONSE,
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

void test_init_to_triggering_state(void)
{
	/* Transition the module into STATE_TRIGGERING */
	send_cloud_connected();

	/* There's an initial transition to STATE_SAMPLE_DATA, where the entry function
	 * sends a location search trigger.
	 */
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Complete location search */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);

	/* FOTA and other sensors are now polled */
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);

	/* Cleanup */
	send_cloud_disconnected();
	expect_no_events(7200);
}

void test_button_press_on_connected(void)
{
	/* Transition to STATE_SAMPLE_DATA */
	send_cloud_connected();
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Transistion to STATE_WAIT_FOR_TRIGGER */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);

	/* Transition back to STATE_SAMPLE_DATA */
	button_handler(DK_BTN1_MSK, DK_BTN1_MSK);
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Cleanup */
	send_cloud_disconnected();
	purge_all_events();

	expect_no_events(7200);
}

void test_button_press_on_disconnected(void)
{
	/* Given */
	send_cloud_disconnected();

	/* When */
	button_handler(DK_BTN1_MSK, DK_BTN1_MSK);
	k_sleep(K_SECONDS(5));

	/* Then */
	expect_no_events(7200);
}

void test_trigger_interval_change_in_connected(void)
{
	/* Transition to STATE_SAMPLE_DATA */
	send_cloud_connected();
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* As response to the shadow poll, the interval is set to 12 hours. */
	twelve_hour_interval_set();

	/* Transition to STATE_TRIGGER_WAIT where shadow is polled. */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);

	/* Wait for the interval to alomost expire and ensure no events are triggered in
	 * that time. Repeat this 10 times.
	 */
	for (int i = 0; i < 10; i++) {
		int elapsed_time;

		elapsed_time = wait_for_location_event(LOCATION_SEARCH_TRIGGER,
						       HOUR_IN_SECONDS * 12);
		TEST_ASSERT_INT_WITHIN(1, HOUR_IN_SECONDS * 12, elapsed_time);

		send_location_search_done();
		expect_location_event(LOCATION_SEARCH_DONE);
		expect_fota_event(FOTA_POLL_REQUEST);
		expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
		expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
	}

	/* Cleanup */
	send_cloud_disconnected();
	expect_no_events(WEEK_IN_SECONDS);
}

void test_trigger_disconnect_and_connect_when_triggering(void)
{
	bool first_trigger_after_connect = true;

	/* Transition to STATE_SAMPLE_DATA */
	send_cloud_connected();

	/* As response to the shadow poll, the interval is set to 12 hours. */
	twelve_hour_interval_set();

	/* Wait for the interval to alomost expire and ensure no events are triggered in
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
		expect_fota_event(FOTA_POLL_REQUEST);
		expect_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
		expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);

		first_trigger_after_connect = false;

		/* Disconnect and connect every second iteration */
		if (i % 2 == 0) {
			send_cloud_disconnected();
			expect_no_events(7200);
			send_cloud_connected();

			first_trigger_after_connect = true;
		}
	}

	/* Cleanup */
	send_cloud_disconnected();
	expect_no_events(WEEK_IN_SECONDS);
}

void test_fota_downloading(void)
{
	/* Transition to STATE_FOTA_DOWNLOADING */
	send_fota_msg(FOTA_DOWNLOADING_UPDATE);
	expect_fota_event(FOTA_DOWNLOADING_UPDATE);

	/* A cloud ready message and button trigger should now cause no action */
	send_cloud_connected();
	expect_no_events(7200);
	button_handler(DK_BTN1_MSK, DK_BTN1_MSK);
	expect_no_events(7200);

	/* Cleanup */
	send_fota_msg(FOTA_DOWNLOAD_CANCELED);
	expect_fota_event(FOTA_DOWNLOAD_CANCELED);

	expect_no_events(7200);
}

void test_fota_waiting_for_network_disconnect(void)
{
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

	/* Give the system time to reboot */
	k_sleep(K_SECONDS(10));

	TEST_ASSERT_EQUAL(1, sys_reboot_fake.call_count);

	/* Cleanup */
	send_fota_msg(FOTA_DOWNLOAD_CANCELED);
	expect_fota_event(FOTA_DOWNLOAD_CANCELED);

	expect_no_events(7200);
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
