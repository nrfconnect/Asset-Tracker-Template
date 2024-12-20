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
#include "modem/lte_lc.h"
#include "modem/modem_info.h"
#include "zephyr/net/net_mgmt.h"

#include "message_channel.h"

#include "zcbor_decode.h"
#include "conn_info_object_decode.h"

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(int, date_time_now, int64_t *);
FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);
FAKE_VALUE_FUNC(int, lte_lc_conn_eval_params_get, struct lte_lc_conn_eval_params *);
FAKE_VALUE_FUNC(int, conn_mgr_all_if_connect, bool);
FAKE_VALUE_FUNC(int, conn_mgr_all_if_up, bool);
FAKE_VOID_FUNC(net_mgmt_add_event_callback, struct net_mgmt_event_callback *);

LOG_MODULE_REGISTER(network_module_test, 4);

ZBUS_MSG_SUBSCRIBER_DEFINE(test_subscriber);
ZBUS_CHAN_ADD_OBS(PAYLOAD_CHAN, test_subscriber, 0);

#define FAKE_TIME_MS 1723099642000
#define FAKE_RSRP_IDX_MAX 97
#define FAKE_RSRP_IDX_INVALID 255
#define FAKE_RSRP_IDX 0
#define FAKE_RSRP_IDX_MIN -17
#define FAKE_ENERGY_ESTIMATE_MAX 9
#define FAKE_ENERGY_ESTIMATE 7
#define FAKE_ENERGY_ESTIMATE_MIN 5

static int date_time_now_custom_fake(int64_t *time)
{
	*time = FAKE_TIME_MS;
	return 0;
}

static int lte_lc_conn_eval_params_get_custom_fake(struct lte_lc_conn_eval_params *params)
{
	params->energy_estimate = FAKE_ENERGY_ESTIMATE;
	params->rsrp = FAKE_RSRP_IDX;

	return 0;
}

static int lte_lc_conn_eval_params_get_max_custom_fake(struct lte_lc_conn_eval_params *params)
{
	params->energy_estimate = FAKE_ENERGY_ESTIMATE_MAX;
	params->rsrp = FAKE_RSRP_IDX_MAX;

	return 0;
}

static int lte_lc_conn_eval_params_get_invalid_custom_fake(struct lte_lc_conn_eval_params *params)
{
	params->energy_estimate = FAKE_ENERGY_ESTIMATE_MAX;
	params->rsrp = FAKE_RSRP_IDX_INVALID;

	return 0;
}

static int lte_lc_conn_eval_params_get_min_custom_fake(struct lte_lc_conn_eval_params *params)
{
	params->energy_estimate = FAKE_ENERGY_ESTIMATE_MIN;
	params->rsrp = FAKE_RSRP_IDX_MIN;

	return 0;
}

static void send_time_available(void)
{
	enum time_status time_type = TIME_AVAILABLE;
	int err = zbus_chan_pub(&TIME_CHAN, &time_type, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_trigger(void)
{
	enum trigger_type trigger_type = TRIGGER_DATA_SAMPLE;
	int err = zbus_chan_pub(&TRIGGER_CHAN, &trigger_type, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void wait_for_and_decode_payload(struct conn_info_object *conn_info_obj)
{
	const struct zbus_channel *chan;
	static struct payload payload;
	int err;

	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_MSEC(100));

	err = zbus_sub_wait_msg(&test_subscriber, &chan, &payload, K_MSEC(1000));
	if (err == -ENOMSG) {
		LOG_ERR("No payload message received");
		TEST_FAIL();
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	/* check if chan is payload channel */
	if (chan != &PAYLOAD_CHAN) {
		LOG_ERR("Received message from wrong channel");
		TEST_FAIL();
	}

	/* decode payload */
	cbor_decode_conn_info_object(payload.buffer, payload.buffer_len, conn_info_obj, NULL);
	if (err != ZCBOR_SUCCESS) {
		LOG_ERR("Failed to decode payload");
		TEST_FAIL();
	}
}

void setUp(void)
{
	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(task_wdt_add);
	RESET_FAKE(date_time_now);
	RESET_FAKE(lte_lc_conn_eval_params_get);
	RESET_FAKE(conn_mgr_all_if_connect);
	RESET_FAKE(conn_mgr_all_if_up);

	date_time_now_fake.custom_fake = date_time_now_custom_fake;
	send_time_available();
}

void tearDown(void)
{
	const struct zbus_channel *chan;
	static struct payload received_payload;
	int err;

	err = zbus_sub_wait_msg(&test_subscriber, &chan, &received_payload, K_MSEC(1000));
	if (err == 0) {
		LOG_ERR("Unhandled message in payload channel");
		TEST_FAIL();
	}
}


void test_energy_estimate(void)
{
	static struct conn_info_object conn_info_obj = {0};

	/* Given */
	lte_lc_conn_eval_params_get_fake.custom_fake = lte_lc_conn_eval_params_get_custom_fake;

	/* When */
	send_trigger();

	/* Then */
	wait_for_and_decode_payload(&conn_info_obj);
	TEST_ASSERT_EQUAL(FAKE_TIME_MS / 1000, conn_info_obj.base_attributes_m.bt);
	TEST_ASSERT_EQUAL(FAKE_ENERGY_ESTIMATE, conn_info_obj.energy_estimate_m.vi);
	TEST_ASSERT_EQUAL(RSRP_IDX_TO_DBM(FAKE_RSRP_IDX), conn_info_obj.rsrp_m.vi.vi);
}

void test_conn_info_max_values(void)
{
	static struct conn_info_object conn_info_obj = {0};

	/* Given */
	lte_lc_conn_eval_params_get_fake.custom_fake = lte_lc_conn_eval_params_get_max_custom_fake;

	/* When */
	send_trigger();

	/* Then */
	wait_for_and_decode_payload(&conn_info_obj);
	TEST_ASSERT_EQUAL(FAKE_TIME_MS / 1000, conn_info_obj.base_attributes_m.bt);
	TEST_ASSERT_EQUAL(FAKE_ENERGY_ESTIMATE_MAX, conn_info_obj.energy_estimate_m.vi);
	TEST_ASSERT_EQUAL(RSRP_IDX_TO_DBM(FAKE_RSRP_IDX_MAX), conn_info_obj.rsrp_m.vi.vi);
}

void test_conn_info_invalid_values(void)
{
	static struct conn_info_object conn_info_obj = {0};

	/* Given */
	lte_lc_conn_eval_params_get_fake.custom_fake =
		lte_lc_conn_eval_params_get_invalid_custom_fake;

	/* When */
	send_trigger();

	/* Then */
	wait_for_and_decode_payload(&conn_info_obj);
	TEST_ASSERT_EQUAL(FAKE_TIME_MS / 1000, conn_info_obj.base_attributes_m.bt);
	TEST_ASSERT_EQUAL(FAKE_ENERGY_ESTIMATE_MAX, conn_info_obj.energy_estimate_m.vi);

	if (conn_info_obj.rsrp_m.vi_present) {
		TEST_FAIL();
	}
}

void test_conn_info_min_values(void)
{
	static struct conn_info_object conn_info_obj = {0};

	/* Given */
	lte_lc_conn_eval_params_get_fake.custom_fake = lte_lc_conn_eval_params_get_min_custom_fake;

	/* When */
	send_trigger();

	/* Then */
	wait_for_and_decode_payload(&conn_info_obj);
	TEST_ASSERT_EQUAL(FAKE_TIME_MS / 1000, conn_info_obj.base_attributes_m.bt);
	TEST_ASSERT_EQUAL(FAKE_ENERGY_ESTIMATE_MIN, conn_info_obj.energy_estimate_m.vi);
	TEST_ASSERT_EQUAL(RSRP_IDX_TO_DBM(FAKE_RSRP_IDX_MIN), conn_info_obj.rsrp_m.vi.vi);
}

void test_no_events_on_zbus_until_watchdog_timeout(void)
{
	/* Wait without feeding any events to zbus until watch dog timeout. */
	k_sleep(K_SECONDS(CONFIG_APP_NETWORK_WATCHDOG_TIMEOUT_SECONDS));

	/* Check if the watchdog was fed atleast once.*/
	TEST_ASSERT_GREATER_OR_EQUAL(1, task_wdt_feed_fake.call_count);
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
