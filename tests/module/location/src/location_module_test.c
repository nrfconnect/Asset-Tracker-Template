/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <unity.h>
#include <zephyr/fff.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <modem/location.h>
#include <modem/lte_lc.h>

#include "app_common.h"
#include "location.h"
#include "network.h"

LOG_MODULE_REGISTER(location_module_test, LOG_LEVEL_DBG);

/* Define subscriber for this module */
ZBUS_MSG_SUBSCRIBER_DEFINE(test_subscriber);

ZBUS_CHAN_DEFINE(NETWORK_CHAN,
	 struct network_msg,
	 NULL,
	 NULL,
	 ZBUS_OBSERVERS_EMPTY,
	 ZBUS_MSG_INIT(.type = NETWORK_DISCONNECTED)
);

/* Add observers for the channels */
ZBUS_CHAN_ADD_OBS(LOCATION_CHAN, test_subscriber, 0);
ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, test_subscriber, 0);

/* Create fakes for required functions */
FAKE_VALUE_FUNC(int, location_init, location_event_handler_t);

/* Store the registered handler */
static location_event_handler_t registered_handler;

/* Custom fake for location_init to store the handler */
static int custom_location_init(location_event_handler_t handler)
{
	registered_handler = handler;
	return 0;
}
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);
FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, lte_lc_func_mode_set, enum lte_lc_func_mode);

FAKE_VALUE_FUNC(int, location_request, const struct location_config *);
FAKE_VALUE_FUNC(int, date_time_set, const struct tm *);
FAKE_VALUE_FUNC(const char *, location_method_str, enum location_method);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_location_send, const struct nrf_cloud_gnss_data *, bool);


/* Define FFF globals */
DEFINE_FFF_GLOBALS;

void setUp(void)
{
	/* Reset all fake functions before each test */
	RESET_FAKE(location_init);
	RESET_FAKE(task_wdt_add);
	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(lte_lc_func_mode_set);
	RESET_FAKE(location_request);
	RESET_FAKE(date_time_set);
	RESET_FAKE(location_method_str);
	RESET_FAKE(nrf_cloud_coap_location_send);

	/* Set up location_init to store the handler */
	location_init_fake.custom_fake = custom_location_init;

	/* Make task_wdt functions return success by default */
	task_wdt_add_fake.return_val = 1;
	task_wdt_feed_fake.return_val = 0;
	lte_lc_func_mode_set_fake.return_val = 0;
}


/* Test that module initialization calls location_init */
void test_module_init(void)
{
	/* Wait a bit for the module thread to start and run state_running_entry */
	k_sleep(K_MSEC(100));

	/* Verify location_init was called by the module */
	TEST_ASSERT_EQUAL(1, location_init_fake.call_count);
}

/* Test GNSS initialization when network connects */
void test_gnss_init_on_network_connect(void)
{
	struct network_msg msg = {
		.type = NETWORK_CONNECTED
	};

	/* Publish network connected message */
	zbus_chan_pub(&NETWORK_CHAN, &msg, K_NO_WAIT);

	/* Give the module time to process */
	k_sleep(K_MSEC(100));

	/* Verify GNSS was initialized */
	TEST_ASSERT_EQUAL(1, lte_lc_func_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_FUNC_MODE_ACTIVATE_GNSS,
	 lte_lc_func_mode_set_fake.arg0_val);
}

/* Test location search trigger handling */
void test_location_search_trigger(void)
{
	enum location_msg_type msg = LOCATION_SEARCH_TRIGGER;

	/* Reset location_request fake */
	RESET_FAKE(location_request);
	location_request_fake.return_val = 0;

	/* Publish trigger message */
	zbus_chan_pub(&LOCATION_CHAN, &msg, K_NO_WAIT);

	/* Give the module time to process */
	k_sleep(K_MSEC(100));

	/* Verify location request was made */
	TEST_ASSERT_EQUAL(1, location_request_fake.call_count);
	TEST_ASSERT_NULL(location_request_fake.arg0_val); /* No config provided */
}

/* Unity requirement */
extern int unity_main(void);

int main(void)
{
	(void)unity_main();
	return 0;
}
