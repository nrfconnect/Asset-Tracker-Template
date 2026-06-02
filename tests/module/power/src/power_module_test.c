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
#include "redef.h"
#include <modem/lte_lc.h>

#include "app_common.h"
#include "power.h"

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);
FAKE_VALUE_FUNC(int, nrf_fuel_gauge_process, float, float, float, float, float *,
		struct nrf_fuel_gauge_state_info *);
FAKE_VALUE_FUNC(int, nrf_fuel_gauge_init, const struct nrf_fuel_gauge_init_parameters *, float *);
FAKE_VALUE_FUNC(int, date_time_now, int64_t *);
FAKE_VALUE_FUNC(int, nrf_fuel_gauge_state_get, void *, size_t);
FAKE_VALUE_FUNC(int, nrf_fuel_gauge_ext_state_update,
		enum nrf_fuel_gauge_ext_state_info_type,
		union nrf_fuel_gauge_ext_state_info_data *);
FAKE_VALUE_FUNC(int, nrf_fuel_gauge_idle_set, float, float, float);
FAKE_VOID_FUNC(lte_lc_register_handler, lte_lc_evt_handler_t);

/* Define nrf_fuel_gauge_state_size for tests (normally provided by the library) */
const size_t nrf_fuel_gauge_state_size = 512;

ZBUS_MSG_SUBSCRIBER_DEFINE(power_subscriber);
ZBUS_CHAN_ADD_OBS(power_chan, power_subscriber, 0);

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
	RESET_FAKE(nrf_fuel_gauge_state_get);
	RESET_FAKE(nrf_fuel_gauge_init);
	RESET_FAKE(nrf_fuel_gauge_process);
	RESET_FAKE(nrf_fuel_gauge_ext_state_update);
	RESET_FAKE(nrf_fuel_gauge_idle_set);
	/* NOTE: lte_lc_register_handler is intentionally NOT reset here.
	 * The handler is registered once in state_running_entry() on the first
	 * modem init. Since the power module thread never re-enters STATE_RUNNING,
	 * resetting this fake would clear arg0_val to NULL and crash the modem
	 * sleep test helpers that invoke the stored handler.
	 */

	/* Set default return values */
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

	if (chan != &power_chan) {
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

	int err = zbus_chan_pub(&power_chan, &msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

void test_module_init(void)
{
	/* Verify that the module initialized successfully by checking that
	 * nrf_fuel_gauge_init was called
	 */
	TEST_ASSERT_EQUAL(1, nrf_fuel_gauge_init_fake.call_count);
	TEST_ASSERT_EQUAL(1, lte_lc_register_handler_fake.call_count);

	/* Verify that the module published a ready message */
	check_power_event(POWER_MODULE_READY);
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

static void send_modem_sleep_entry(void)
{
	lte_lc_evt_handler_t handler = lte_lc_register_handler_fake.arg0_val;
	struct lte_lc_evt evt = {
		.type = (enum lte_lc_evt_type)LTE_LC_EVT_MODEM_SLEEP_ENTER,
	};

	handler(&evt);
}

static void send_modem_sleep_exit(void)
{
	lte_lc_evt_handler_t handler = lte_lc_register_handler_fake.arg0_val;
	struct lte_lc_evt evt = {
		.type = (enum lte_lc_evt_type)LTE_LC_EVT_MODEM_SLEEP_EXIT,
	};

	handler(&evt);
}

void test_fuel_gauge_state_saved_periodically(void)
{
	/* The fuel gauge state is saved by the periodic timer (sample_and_process),
	 * not by POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST. The timer fires every
	 * CONFIG_APP_POWER_SAMPLE_INTERVAL_MS (1000ms in test).
	 */

	/* Wait for at least 2 timer expirations */
	k_sleep(K_MSEC(2100));

	/* State should have been saved at least twice */
	TEST_ASSERT_GREATER_OR_EQUAL(2, nrf_fuel_gauge_state_get_fake.call_count);
}

void test_fuel_gauge_state_save_error_handling(void)
{
	/* Given - Set nrf_fuel_gauge_state_get to fail */
	nrf_fuel_gauge_state_get_fake.return_val = -EIO;

	/* When - Request battery percentage sample */
	send_power_battery_percentage_sample_request();
	k_sleep(K_MSEC(1000));

	/* Then - Module should still produce a response even if state save fails */
	check_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
	check_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE);
}

void test_fuel_gauge_state_saved_multiple_times_by_timer(void)
{
	/* Wait for N timer expirations and verify state is saved each time */
	k_sleep(K_MSEC(5100));

	/* State should be saved at least 5 times (once per timer firing) */
	TEST_ASSERT_GREATER_OR_EQUAL(5, nrf_fuel_gauge_state_get_fake.call_count);
}

void test_modem_sleep_entry_transitions_to_idle(void)
{
	/* Given - Module is in STATE_ACTIVE after init; reset idle_set counter */
	RESET_FAKE(nrf_fuel_gauge_idle_set);

	/* When - Modem sleep entry event */
	send_modem_sleep_entry();
	k_sleep(K_MSEC(100));

	/* Then - idle_set should have been called once (in state_idle_entry) */
	TEST_ASSERT_EQUAL(1, nrf_fuel_gauge_idle_set_fake.call_count);

	/* And - No further samples should be taken while in IDLE state
	 * (timer was stopped by state_active_exit)
	 */
	RESET_FAKE(nrf_fuel_gauge_state_get);
	k_sleep(K_SECONDS(3));
	TEST_ASSERT_EQUAL(0, nrf_fuel_gauge_state_get_fake.call_count);
}

void test_modem_sleep_exit_transitions_to_active(void)
{
	/* Given - Module is in STATE_IDLE */
	send_modem_sleep_entry();
	k_sleep(K_MSEC(100));

	/* Reset state counters before testing the transition back */
	RESET_FAKE(nrf_fuel_gauge_idle_set);
	RESET_FAKE(nrf_fuel_gauge_state_get);

	/* When - Modem sleep exit event */
	send_modem_sleep_exit();
	k_sleep(K_MSEC(100));

	/* Then - idle_set should NOT be called again (now in STATE_ACTIVE) */
	TEST_ASSERT_EQUAL(0, nrf_fuel_gauge_idle_set_fake.call_count);

	/* And - Periodic sampling should resume (timer restarted in state_active_entry) */
	k_sleep(K_SECONDS(2));
	TEST_ASSERT_GREATER_OR_EQUAL(1, nrf_fuel_gauge_state_get_fake.call_count);
}

void test_idle_state_ignores_additional_sleep_entry(void)
{
	/* Given - Module is already in STATE_IDLE */
	send_modem_sleep_entry();
	k_sleep(K_MSEC(100));
	RESET_FAKE(nrf_fuel_gauge_idle_set);

	/* When - A duplicate sleep entry event arrives */
	send_modem_sleep_entry();
	k_sleep(K_MSEC(100));

	/* Then - idle_set should NOT be called again */
	TEST_ASSERT_EQUAL(0, nrf_fuel_gauge_idle_set_fake.call_count);
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
