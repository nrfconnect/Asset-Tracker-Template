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
#include "message_channel.h"
#include "battery.h"

#include "checks.h"

/* Define the channels for testing */
ZBUS_CHAN_DEFINE(BATTERY_CHAN,
		 struct battery_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);
ZBUS_CHAN_DEFINE(BUTTON_CHAN,
		 struct battery_msg,
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
FAKE_VALUE_FUNC(int, nrf_cloud_client_id_get, char *, size_t);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_init);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_connect, const char * const);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_disconnect);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_shadow_device_status_update);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_bytes_send, uint8_t *, size_t, bool);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_sensor_send, const char *, double, int64_t, bool);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_json_message_send, const char *, bool, bool);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_shadow_get, char *, size_t *, bool, int);

LOG_MODULE_REGISTER(trigger_module_test, 4);

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

static void send_cloud_connected_ready_to_send(void)
{
	enum cloud_msg_type status = CLOUD_CONNECTED_READY_TO_SEND;
	int err = zbus_chan_pub(&CLOUD_CHAN, &status, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_config(uint64_t interval)
{
	const struct configuration config = {
		.update_interval_present = true,
		.update_interval = interval,
	};

	int err = zbus_chan_pub(&CONFIG_CHAN, &config, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_cloud_disconnected(void)
{
	enum cloud_msg_type status = CLOUD_DISCONNECTED;
	int err = zbus_chan_pub(&CLOUD_CHAN, &status, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_time_available(void)
{
	enum time_status time_type = TIME_AVAILABLE;
	int err = zbus_chan_pub(&TIME_CHAN, &time_type, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

void test_init_to_connected_state(void)
{
	/* Given */
	send_time_available();

	k_sleep(K_SECONDS(1));

	send_cloud_connected_ready_to_send();

	/* When */
	k_sleep(K_SECONDS(HOUR_IN_SECONDS));

	/* Then */
	uint32_t interval =  HOUR_IN_SECONDS / CONFIG_APP_MODULE_TRIGGER_TIMEOUT_SECONDS;

	check_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
	check_battery_event(BATTERY_PERCENTAGE_SAMPLE_REQUEST);

	for (int i = 0; i < interval; i++) {
		check_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
		check_battery_event(BATTERY_PERCENTAGE_SAMPLE_REQUEST);
	}

	/* Cleanup */
	send_cloud_disconnected();
	check_no_events(7200);
}

void test_button_press_on_connected(void)
{
	/* Given */
	send_cloud_connected_ready_to_send();

	/* When */
	button_handler(DK_BTN1_MSK, DK_BTN1_MSK);
	k_sleep(K_SECONDS(5));


	/* Then */

	/* Two events, one for initial trigger when entering connected state and one for the
	 * button press
	 */
	check_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
	check_battery_event(BATTERY_PERCENTAGE_SAMPLE_REQUEST);
	check_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
	check_battery_event(BATTERY_PERCENTAGE_SAMPLE_REQUEST);

	/* Cleanup */
	send_cloud_disconnected();
	check_no_events(7200);
}

void test_button_press_on_disconnected(void)
{
	/* Given */
	send_cloud_disconnected();

	/* When */
	button_handler(DK_BTN1_MSK, DK_BTN1_MSK);
	k_sleep(K_SECONDS(5));

	/* Then */
	check_no_events(7200);
}

void test_trigger_interval_change_in_connected(void)
{
	/* Given */
	send_cloud_connected_ready_to_send();
	send_config(HOUR_IN_SECONDS * 12);

	/* When */
	k_sleep(K_SECONDS(WEEK_IN_SECONDS));

	/* Then */
	uint32_t interval =  (WEEK_IN_SECONDS) / (HOUR_IN_SECONDS * 12);

	check_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
	check_battery_event(BATTERY_PERCENTAGE_SAMPLE_REQUEST);

	for (int i = 0; i < interval; i++) {
		check_network_event(NETWORK_QUALITY_SAMPLE_REQUEST);
		check_battery_event(BATTERY_PERCENTAGE_SAMPLE_REQUEST);
	}

	/* Cleanup */
	send_cloud_disconnected();
	check_no_events((WEEK_IN_SECONDS));
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
