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
#include <modem/lte_lc.h>
#include <modem/modem_info.h>
#include <zephyr/net/net_mgmt.h>

#include "message_channel.h"

LOG_MODULE_REGISTER(network_module_test, 4);

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(int, date_time_now, int64_t *);
FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);
FAKE_VALUE_FUNC(int, lte_lc_conn_eval_params_get, struct lte_lc_conn_eval_params *);
FAKE_VALUE_FUNC(int, conn_mgr_all_if_connect, bool);
FAKE_VALUE_FUNC(int, conn_mgr_all_if_up, bool);
FAKE_VALUE_FUNC(int, conn_mgr_all_if_disconnect, bool);
FAKE_VOID_FUNC(net_mgmt_add_event_callback, struct net_mgmt_event_callback *);
FAKE_VOID_FUNC(lte_lc_register_handler, lte_lc_evt_handler_t);
FAKE_VALUE_FUNC(int, lte_lc_modem_events_enable);
FAKE_VALUE_FUNC(int, lte_lc_system_mode_get, enum lte_lc_system_mode *,
		enum lte_lc_system_mode_preference *);

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
#define FAKE_SYSTEM_MODE		LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS

static lte_lc_evt_handler_t lte_evt_handler;
static struct net_mgmt_event_callback net_mgmt_evt_cb;

/* Forward declarations */
static void send_l4_evt(uint32_t mgmt_event);

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

static int conn_mgr_all_if_connect_custom_fake(bool unused)
{
	ARG_UNUSED(unused);

	TEST_ASSERT_NOT_NULL(net_mgmt_evt_cb.handler);

	send_l4_evt(NET_EVENT_L4_CONNECTED);

	return 0;
}

static int conn_mgr_all_if_disconnect_custom_fake(bool unused)
{
	ARG_UNUSED(unused);

	TEST_ASSERT_NOT_NULL(net_mgmt_evt_cb.handler);

	send_l4_evt(NET_EVENT_L4_DISCONNECTED);

	return 0;
}

static void net_mgmt_add_event_callback_custom_fake(struct net_mgmt_event_callback *cb)
{
	/* We only want to hook into the L4 callback */
	if (cb->event_mask == (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)) {

		net_mgmt_evt_cb = *cb;
	}
}

static int lte_lc_system_mode_get_custom_fake(enum lte_lc_system_mode *mode,
					       enum lte_lc_system_mode_preference *preference)
{
	*mode = FAKE_SYSTEM_MODE;

	return 0;
}

static void send_l4_evt(uint32_t mgmt_event)
{
	TEST_ASSERT_NOT_NULL(net_mgmt_evt_cb.handler);

	net_mgmt_evt_cb.handler(&net_mgmt_evt_cb, mgmt_event, NULL);
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

static void send_mdmev_evt(enum lte_lc_modem_evt evt)
{
	struct lte_lc_evt lte_evt = {
		.type = LTE_LC_EVT_MODEM_EVENT,
		.modem_evt = evt,
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
	RESET_FAKE(conn_mgr_all_if_connect);
	RESET_FAKE(conn_mgr_all_if_up);
	RESET_FAKE(net_mgmt_add_event_callback);

	date_time_now_fake.custom_fake = date_time_now_custom_fake;
	lte_lc_register_handler_fake.custom_fake = lte_lc_register_handler_custom_fake;
	conn_mgr_all_if_connect_fake.custom_fake = conn_mgr_all_if_connect_custom_fake;
	conn_mgr_all_if_disconnect_fake.custom_fake = conn_mgr_all_if_disconnect_custom_fake;
	net_mgmt_add_event_callback_fake.custom_fake = net_mgmt_add_event_callback_custom_fake;
	lte_lc_conn_eval_params_get_fake.custom_fake = lte_lc_conn_eval_params_get_custom_fake;
	lte_lc_system_mode_get_fake.custom_fake = lte_lc_system_mode_get_custom_fake;

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

	send_l4_evt(NET_EVENT_L4_DISCONNECTED);

	wait_for_and_check_msg(&msg, NETWORK_DISCONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_DISCONNECTED, msg.type);
}

void test_network_connected(void)
{
	struct network_msg msg;

	send_l4_evt(NET_EVENT_L4_CONNECTED);

	wait_for_and_check_msg(&msg, NETWORK_CONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_CONNECTED, msg.type);
}

void test_energy_estimate(void)
{
	struct network_msg msg;

	lte_lc_conn_eval_params_get_fake.custom_fake = lte_lc_conn_eval_params_get_custom_fake;

	/* Network quality can only be sampled when connected */
	send_l4_evt(NET_EVENT_L4_CONNECTED);
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

	wait_for_and_check_msg(&msg, NETWORK_LIGHT_SERACH_DONE);

	TEST_ASSERT_EQUAL(NETWORK_LIGHT_SERACH_DONE, msg.type);
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
	TEST_ASSERT_EQUAL(FAKE_SYSTEM_MODE, msg.system_mode);
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
