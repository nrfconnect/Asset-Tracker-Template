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
#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
#include <modem/modem_info.h>

#include "app_common.h"
#include "network.h"

LOG_MODULE_REGISTER(network_module_test, 4);

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(int, nrf_modem_lib_init);
FAKE_VALUE_FUNC(int, date_time_now, int64_t *);
FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);
FAKE_VALUE_FUNC(int, lte_lc_conn_eval_params_get, struct lte_lc_conn_eval_params *);
FAKE_VOID_FUNC(lte_lc_register_handler, lte_lc_evt_handler_t);
FAKE_VALUE_FUNC(int, lte_lc_modem_events_enable);
FAKE_VALUE_FUNC(int, lte_lc_system_mode_get, enum lte_lc_system_mode *,
		enum lte_lc_system_mode_preference *);
FAKE_VALUE_FUNC(int, lte_lc_system_mode_set, enum lte_lc_system_mode,
		enum lte_lc_system_mode_preference);
FAKE_VALUE_FUNC(int, lte_lc_offline);
FAKE_VALUE_FUNC(int, lte_lc_connect_async, lte_lc_evt_handler_t);
FAKE_VALUE_FUNC(int, lte_lc_pdn_default_ctx_events_enable);

ZBUS_MSG_SUBSCRIBER_DEFINE(test_subscriber);
ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, test_subscriber, 0);

#define FAKE_TIME_MS			1723099642000
#define FAKE_RSRP_IDX_MAX		97
#define FAKE_RSRP_IDX_INVALID		255
#define FAKE_RSRP_IDX			28
#define FAKE_RSRP_IDX_MIN		-17
#define FAKE_ENERGY_ESTIMATE_MAX	9
#define FAKE_ENERGY_ESTIMATE		7
#define FAKE_ENERGY_ESTIMATE_MIN	5
#define FAKE_PSM_TAU			3600
#define FAKE_PSM_ACTIVE_TIME		16
#define FAKE_EDRX_VALUE			163.84f
#define FAKE_EDRX_PTW			1.28f
#define FAKE_SYSTEM_MODE_DEFAULT	LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS

static lte_lc_evt_handler_t lte_evt_handler;
static enum lte_lc_system_mode current_fake_system_mode = FAKE_SYSTEM_MODE_DEFAULT;

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

static void lte_lc_register_handler_custom_fake(lte_lc_evt_handler_t handler)
{
	lte_evt_handler = handler;
}

static int lte_lc_pdn_default_ctx_events_enable_custom_fake(void)
{
	/* Simulate initial PDN state as disconnected */
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_PDN,
		.pdn = {
			.type = LTE_LC_EVT_PDN_DEACTIVATED,
			.cid = 0,
		},
	};

	/* Notify the handler of initial disconnected state */
	if (lte_evt_handler) {
		lte_evt_handler(&evt);
	}

	return 0;
}

static int lte_lc_connect_async_custom_fake(lte_lc_evt_handler_t handler)
{
	/* Simulate successful connection by sending PDN activated event */
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_PDN,
		.pdn = {
			.type = LTE_LC_EVT_PDN_ACTIVATED,
			.cid = 0,
		},
	};

	/* Store the handler for later use */
	lte_evt_handler = handler;

	/* Simulate the PDN activation event */
	if (lte_evt_handler) {
		lte_evt_handler(&evt);
	}

	return 0;
}



static int lte_lc_offline_custom_fake(void)
{
	/* Simulate going offline by sending PDN deactivated event */
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_PDN,
		.pdn = {
			.type = LTE_LC_EVT_PDN_DEACTIVATED,
			.cid = 0,
		},
	};

	/* Simulate the PDN deactivation event */
	if (lte_evt_handler) {
		lte_evt_handler(&evt);
	}

	return 0;
}

static int lte_lc_system_mode_get_custom_fake(enum lte_lc_system_mode *mode,
					      enum lte_lc_system_mode_preference *preference)
{
	ARG_UNUSED(preference);

	*mode = current_fake_system_mode;

	return 0;
}

static int lte_lc_system_mode_set_custom_fake(enum lte_lc_system_mode mode,
					      enum lte_lc_system_mode_preference preference)
{
	ARG_UNUSED(preference);

	current_fake_system_mode = mode;

	return 0;
}

static void send_psm_update_evt(void)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_PSM_UPDATE,
		.psm_cfg = {
			.tau = FAKE_PSM_TAU,
			.active_time = FAKE_PSM_ACTIVE_TIME,
		},
	};

	lte_evt_handler(&evt);
}

static void send_edrx_update_evt(void)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_EDRX_UPDATE,
		.edrx_cfg = {
			.mode = LTE_LC_LTE_MODE_LTEM,
			.edrx = FAKE_EDRX_VALUE,
			.ptw = FAKE_EDRX_PTW,
		},
	};

	lte_evt_handler(&evt);
}

static void send_network_attach_rejected(void)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_NW_REG_STATUS,
		.nw_reg_status = LTE_LC_NW_REG_NOT_REGISTERED,
	};

	lte_evt_handler(&evt);
}

static void send_uicc_failure(void)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_NW_REG_STATUS,
		.nw_reg_status = LTE_LC_NW_REG_UICC_FAIL,
	};

	lte_evt_handler(&evt);
}

static void send_mdmev_evt(enum lte_lc_modem_evt_type evt)
{
	struct lte_lc_evt lte_evt = {
		.type = LTE_LC_EVT_MODEM_EVENT,
		.modem_evt.type = evt,
	};

	lte_evt_handler(&lte_evt);
}

static void wait_for_and_check_msg(struct network_msg *msg, enum network_msg_type expected_type)
{
	const struct zbus_channel *chan;
	int err;

	/* Give the test 1500 ms to complete */
	uint64_t end_time = k_uptime_get() + 1500;

	while (k_uptime_get() < end_time) {
		err = zbus_sub_wait_msg(&test_subscriber, &chan, msg, K_MSEC(1000));
		if (err == -ENOMSG) {
			LOG_ERR("No message received");
			TEST_FAIL();
		} else if (err) {
			LOG_ERR("zbus_sub_wait, error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}

		if (chan != &NETWORK_CHAN) {
			LOG_ERR("Received message from wrong channel");
			TEST_FAIL();
		}

		LOG_DBG("Received message type: %d\n", msg->type);

		if (msg->type == expected_type) {
			LOG_DBG("Received expected message type: %d\n", msg->type);

			return;
		}
	}

	LOG_ERR("Timeout waiting for message");
	TEST_FAIL();
}

static void request_nw_quality(void)
{
	struct network_msg msg = { .type = NETWORK_QUALITY_SAMPLE_REQUEST, };
	int err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

void setUp(void)
{
	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(task_wdt_add);
	RESET_FAKE(date_time_now);
	RESET_FAKE(lte_lc_conn_eval_params_get);
	RESET_FAKE(lte_lc_offline);
	RESET_FAKE(lte_lc_connect_async);
	RESET_FAKE(lte_lc_pdn_default_ctx_events_enable);
	RESET_FAKE(lte_lc_modem_events_enable);
	RESET_FAKE(lte_lc_system_mode_get);
	RESET_FAKE(lte_lc_system_mode_set);
	RESET_FAKE(lte_lc_register_handler);
	RESET_FAKE(nrf_modem_lib_init);


	date_time_now_fake.custom_fake = date_time_now_custom_fake;
	lte_lc_register_handler_fake.custom_fake = lte_lc_register_handler_custom_fake;
	lte_lc_conn_eval_params_get_fake.custom_fake = lte_lc_conn_eval_params_get_custom_fake;
	lte_lc_system_mode_get_fake.custom_fake = lte_lc_system_mode_get_custom_fake;
	lte_lc_system_mode_set_fake.custom_fake = lte_lc_system_mode_set_custom_fake;
	lte_lc_connect_async_fake.custom_fake = lte_lc_connect_async_custom_fake;
	lte_lc_offline_fake.custom_fake = lte_lc_offline_custom_fake;
	lte_lc_pdn_default_ctx_events_enable_fake.custom_fake =
		lte_lc_pdn_default_ctx_events_enable_custom_fake;

	/* Sleep to allow threads to start */
	k_sleep(K_MSEC(500));
}

void tearDown(void)
{
	const struct zbus_channel *chan;
	static struct network_msg msg;

	while (zbus_sub_wait_msg(&test_subscriber, &chan, &msg, K_MSEC(1000)) == 0) {
		LOG_INF("Unhandled message in channel: %d", msg.type);
	}
}

void test_network_disconnected(void)
{
	struct network_msg msg;

	wait_for_and_check_msg(&msg, NETWORK_DISCONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_DISCONNECTED, msg.type);
}

void test_network_connected(void)
{
	struct network_msg msg_tx = { .type = NETWORK_DISCONNECT };
	struct network_msg msg_rx;
	int err;

	/* First, ensure we are disconnected and idle */
	err = zbus_chan_pub(&NETWORK_CHAN, &msg_tx, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* The test thread needs to yield to allow the message to be processed */
	k_sleep(K_MSEC(10));

	/* Then, trigger connection */
	msg_tx.type = NETWORK_CONNECT;

	err = zbus_chan_pub(&NETWORK_CHAN, &msg_tx, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Now wait for the connected message */
	wait_for_and_check_msg(&msg_rx, NETWORK_CONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_CONNECTED, msg_rx.type);
}

void test_energy_estimate(void)
{
	struct network_msg msg;

	lte_lc_conn_eval_params_get_fake.custom_fake = lte_lc_conn_eval_params_get_custom_fake;

	/* Network quality can only be sampled when connected */
	request_nw_quality();

	wait_for_and_check_msg(&msg, NETWORK_QUALITY_SAMPLE_RESPONSE);

	TEST_ASSERT_EQUAL(NETWORK_QUALITY_SAMPLE_RESPONSE, msg.type);
	TEST_ASSERT_EQUAL(FAKE_ENERGY_ESTIMATE, msg.conn_eval_params.energy_estimate);
	TEST_ASSERT_EQUAL(FAKE_RSRP_IDX, msg.conn_eval_params.rsrp);
}

void test_psm_params_update(void)
{
	struct network_msg msg;

	send_psm_update_evt();

	wait_for_and_check_msg(&msg, NETWORK_PSM_PARAMS);

	TEST_ASSERT_EQUAL(NETWORK_PSM_PARAMS, msg.type);
	TEST_ASSERT_EQUAL(FAKE_PSM_TAU, msg.psm_cfg.tau);
	TEST_ASSERT_EQUAL(FAKE_PSM_ACTIVE_TIME, msg.psm_cfg.active_time);
}

void test_edrx_params_update(void)
{
	struct network_msg msg;

	send_edrx_update_evt();

	wait_for_and_check_msg(&msg, NETWORK_EDRX_PARAMS);

	TEST_ASSERT_EQUAL(NETWORK_EDRX_PARAMS, msg.type);
	TEST_ASSERT_EQUAL(LTE_LC_LTE_MODE_LTEM, msg.edrx_cfg.mode);
	TEST_ASSERT_EQUAL(FAKE_EDRX_VALUE, msg.edrx_cfg.edrx);
	TEST_ASSERT_EQUAL(FAKE_EDRX_PTW, msg.edrx_cfg.ptw);
}

void test_no_events_on_zbus_until_watchdog_timeout(void)
{
	/* Wait without feeding any events to zbus until watch dog timeout. */
	k_sleep(K_SECONDS(CONFIG_APP_NETWORK_WATCHDOG_TIMEOUT_SECONDS));

	/* Check if the watchdog was fed atleast once.*/
	TEST_ASSERT_GREATER_OR_EQUAL(1, task_wdt_feed_fake.call_count);
}

void test_netwowrk_attach_rejected(void)
{
	struct network_msg msg;

	send_network_attach_rejected();

	wait_for_and_check_msg(&msg, NETWORK_ATTACH_REJECTED);

	TEST_ASSERT_EQUAL(NETWORK_ATTACH_REJECTED, msg.type);
}

void test_light_search_done(void)
{
	struct network_msg msg;

	send_mdmev_evt(LTE_LC_MODEM_EVT_LIGHT_SEARCH_DONE);

	wait_for_and_check_msg(&msg, NETWORK_LIGHT_SEARCH_DONE);

	TEST_ASSERT_EQUAL(NETWORK_LIGHT_SEARCH_DONE, msg.type);
}

void test_search_done(void)
{
	struct network_msg msg;

	send_mdmev_evt(LTE_LC_MODEM_EVT_SEARCH_DONE);

	wait_for_and_check_msg(&msg, NETWORK_SEARCH_DONE);

	TEST_ASSERT_EQUAL(NETWORK_SEARCH_DONE, msg.type);
}

void test_modem_reset_loop(void)
{
	struct network_msg msg;

	send_mdmev_evt(LTE_LC_MODEM_EVT_RESET_LOOP);

	wait_for_and_check_msg(&msg, NETWORK_MODEM_RESET_LOOP);

	TEST_ASSERT_EQUAL(NETWORK_MODEM_RESET_LOOP, msg.type);
}

void test_uicc_failure(void)
{
	struct network_msg msg;

	send_uicc_failure();

	wait_for_and_check_msg(&msg, NETWORK_UICC_FAILURE);

	TEST_ASSERT_EQUAL(NETWORK_UICC_FAILURE, msg.type);
}

void test_network_disconnect_reconnect(void)
{
	struct network_msg msg = { .type = NETWORK_DISCONNECT, };
	int err;

	/* First, ensure we are disconnected */
	err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* The test thread needs to yield to allow the message to be processed in the network
	 * module.
	 */
	k_sleep(K_MSEC(10));

	/* Then, connect */
	msg.type = NETWORK_CONNECT;

	err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	wait_for_and_check_msg(&msg, NETWORK_CONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_CONNECTED, msg.type);
}

void test_system_mode_request(void)
{
	struct network_msg msg = { .type = NETWORK_SYSTEM_MODE_REQUEST, };
	int err;

	err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	wait_for_and_check_msg(&msg, NETWORK_SYSTEM_MODE_RESPONSE);
	TEST_ASSERT_EQUAL(NETWORK_SYSTEM_MODE_RESPONSE, msg.type);
	TEST_ASSERT_EQUAL(current_fake_system_mode, msg.system_mode);
}

static void system_mode_set_test(enum network_msg_type msg_type, enum lte_lc_system_mode expected)
{
	struct network_msg msg = { .type = NETWORK_DISCONNECT, };
	int err;

	/* Ensure that the module is disconnected and idle */
	err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* The test thread needs to yield to allow the message to be processed in the network
	 * module.
	 */
	k_sleep(K_MSEC(10));

	msg.type = msg_type;

	err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_MSEC(10));

	/* Request the system mode */
	msg.type = NETWORK_SYSTEM_MODE_REQUEST;

	err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	wait_for_and_check_msg(&msg, NETWORK_SYSTEM_MODE_RESPONSE);
	TEST_ASSERT_EQUAL(NETWORK_SYSTEM_MODE_RESPONSE, msg.type);
	TEST_ASSERT_EQUAL(expected, msg.system_mode);
}

void test_system_mode_set_ltem(void)
{
	system_mode_set_test(NETWORK_SYSTEM_MODE_SET_LTEM, LTE_LC_SYSTEM_MODE_LTEM_GPS);
}

void test_system_mode_set_nbiot(void)
{
	system_mode_set_test(NETWORK_SYSTEM_MODE_SET_NBIOT, LTE_LC_SYSTEM_MODE_NBIOT_GPS);
}

void test_system_mode_set_ltem_nbiot(void)
{
	system_mode_set_test(NETWORK_SYSTEM_MODE_SET_LTEM_NBIOT, LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS);
}

void test_disconnect_while_searching(void)
{
	int err;
	struct network_msg msg = {
		.type = NETWORK_DISCONNECT,
	};

	/* Ensure we are disconnected and idle */
	err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_MSEC(100));

	/* Start searching */
	msg.type = NETWORK_CONNECT;

	err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_MSEC(100));

	/* Stop searching / Disconnect */
	msg.type = NETWORK_DISCONNECT;

	err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Verify we receive the disconnected event confirming the action */
	wait_for_and_check_msg(&msg, NETWORK_DISCONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_DISCONNECTED, msg.type);
}

void test_connect_from_idle(void)
{
	int err;
	struct network_msg msg = {
		.type = NETWORK_DISCONNECT,
	};
	struct network_msg msg_rx;

	/* Ensure we are disconnected and idle */
	err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_MSEC(100));

	/* Send connect request */
	msg.type = NETWORK_CONNECT;

	err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Verify we eventually connect */
	wait_for_and_check_msg(&msg_rx, NETWORK_CONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_CONNECTED, msg_rx.type);
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
