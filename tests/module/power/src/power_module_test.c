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
#include <errno.h>
#include <string.h>
#include <nrf_fuel_gauge.h>

#include "app_common.h"
#include "power.h"

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);
FAKE_VALUE_FUNC(float, nrf_fuel_gauge_process, float, float, float, float,
		struct nrf_fuel_gauge_state_info *);
FAKE_VALUE_FUNC(int, charger_read_sensors, float *, float *, float *, int32_t *);
FAKE_VALUE_FUNC(int, nrf_fuel_gauge_init, const struct nrf_fuel_gauge_init_parameters *, float *);
FAKE_VALUE_FUNC(int, mfd_npm13xx_add_callback, const struct device *, struct gpio_callback *);
FAKE_VALUE_FUNC(int, date_time_now, int64_t *);
FAKE_VALUE_FUNC(int, nrf_modem_lib_trace_level_set, int);
FAKE_VALUE_FUNC(int, nrf_fuel_gauge_state_get, void *, size_t);

/* Define nrf_fuel_gauge_state_size for tests (normally provided by the library) */
const size_t nrf_fuel_gauge_state_size = 128;

ZBUS_MSG_SUBSCRIBER_DEFINE(power_subscriber);
ZBUS_CHAN_ADD_OBS(POWER_CHAN, power_subscriber, 0);

LOG_MODULE_REGISTER(power_module_test, 4);

/* NRF_MODEM_LIB_ON_INIT creates a structure with the callback.
 * We can access it to invoke the modem init callback in tests.
 */
struct nrf_modem_lib_init_cb {
	void (*callback)(int ret, void *ctx);
	void *context;
};
extern struct nrf_modem_lib_init_cb nrf_modem_hook_power_modem_init_hook;

void setUp(void)
{
	/* reset fakes */
	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(task_wdt_add);
	RESET_FAKE(date_time_now);
	RESET_FAKE(nrf_modem_lib_trace_level_set);
	RESET_FAKE(nrf_fuel_gauge_state_get);
	RESET_FAKE(nrf_fuel_gauge_init);
	RESET_FAKE(nrf_fuel_gauge_process);
	RESET_FAKE(charger_read_sensors);
	RESET_FAKE(mfd_npm13xx_add_callback);

	/* Set default return values */
	nrf_modem_lib_trace_level_set_fake.return_val = 0;
	nrf_fuel_gauge_state_get_fake.return_val = 0;

	const struct zbus_channel *chan;
	struct power_msg received_msg;

	while (zbus_sub_wait_msg(&power_subscriber, &chan, &received_msg, K_NO_WAIT) == 0) {
		/* Purge all messages from the channel */
	}

	/* Initialize the power module by calling the modem init callback
	 * via the hook structure
	 */
	nrf_modem_hook_power_modem_init_hook.callback(0,
		nrf_modem_hook_power_modem_init_hook.context);

	/* Wait for initialization */
	k_sleep(K_MSEC(100));
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

void test_module_init(void)
{
	/* Verify that the module initialized successfully by checking that
	 * nrf_fuel_gauge_init was called
	 */
	TEST_ASSERT_EQUAL(1, nrf_fuel_gauge_init_fake.call_count);
	TEST_ASSERT_EQUAL(1, mfd_npm13xx_add_callback_fake.call_count);
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

void test_fuel_gauge_state_saved_after_sample(void)
{
	/* When - Request battery percentage sample */
	send_power_battery_percentage_sample_request();
	k_sleep(K_SECONDS(1));

	/* Then - Verify fuel gauge state was saved */
	check_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
	check_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE);

	/* State should be saved exactly once after the sample */
	TEST_ASSERT_EQUAL(1, nrf_fuel_gauge_state_get_fake.call_count);
}

void test_fuel_gauge_state_save_error_handling(void)
{
	/* Given - Set nrf_fuel_gauge_state_get to fail */
	nrf_fuel_gauge_state_get_fake.return_val = -EIO;

	/* When - Request battery percentage sample */
	send_power_battery_percentage_sample_request();
	k_sleep(K_SECONDS(1));

	/* Then - Module should still produce response even if state save fails */
	check_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
	check_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE);

	/* Verify state_get was attempted exactly once */
	TEST_ASSERT_EQUAL(1, nrf_fuel_gauge_state_get_fake.call_count);
}

void test_fuel_gauge_state_multiple_samples_save_state(void)
{
	/* When - Request multiple battery samples */
	for (int i = 0; i < 5; i++) {
		send_power_battery_percentage_sample_request();
		k_sleep(K_SECONDS(1));
		check_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
		check_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE);
	}

	/* Then - State should be saved exactly once per sample (5 calls total) */
	TEST_ASSERT_EQUAL(5, nrf_fuel_gauge_state_get_fake.call_count);
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
