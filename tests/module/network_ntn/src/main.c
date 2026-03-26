/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
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
#include <modem/ntn.h>

#include "app_common.h"
#include "network.h"

LOG_MODULE_REGISTER(network_ntn_test, 4);

#define FAKE_PSM_TAU		3600
#define FAKE_PSM_ACTIVE_TIME	16
#define FAKE_EDRX_VALUE		163.84f
#define FAKE_EDRX_PTW		1.28f

#define NTN_LAT			63.430000
#define NTN_LON			10.390000
#define NTN_ALT			50.0f

DEFINE_FFF_GLOBALS;

/* Modem / system fakes */
FAKE_VALUE_FUNC(int, nrf_modem_lib_init);
FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);
FAKE_VOID_FUNC(lte_lc_register_handler, lte_lc_evt_handler_t);
FAKE_VALUE_FUNC(int, lte_lc_system_mode_get,
		enum lte_lc_system_mode *,
		enum lte_lc_system_mode_preference *);
FAKE_VALUE_FUNC(int, lte_lc_system_mode_set, enum lte_lc_system_mode,
		enum lte_lc_system_mode_preference);
FAKE_VALUE_FUNC(int, lte_lc_offline);
FAKE_VALUE_FUNC(int, lte_lc_connect_async, lte_lc_evt_handler_t);
FAKE_VALUE_FUNC(int, lte_lc_pdn_default_ctx_events_enable);
FAKE_VALUE_FUNC(int, lte_lc_func_mode_set, enum lte_lc_func_mode);

/* NTN library fakes */
FAKE_VOID_FUNC(ntn_register_handler, ntn_evt_handler_t);
FAKE_VALUE_FUNC(int, ntn_location_set, double, double, float, uint32_t);
FAKE_VALUE_FUNC(int, ntn_location_invalidate);

/* Test subscriber that observes network_chan output */
ZBUS_MSG_SUBSCRIBER_DEFINE(test_subscriber);
ZBUS_CHAN_ADD_OBS(network_chan, test_subscriber, 0);

static lte_lc_evt_handler_t lte_evt_handler;
static ntn_evt_handler_t ntn_evt_handler_cb;

static enum lte_lc_system_mode current_sys_mode;

/* Custom fakes */

static void lte_lc_register_handler_custom(lte_lc_evt_handler_t handler)
{
	lte_evt_handler = handler;
}

static int lte_lc_pdn_default_ctx_events_enable_custom(void)
{
	return 0;
}

static int lte_lc_connect_async_custom(lte_lc_evt_handler_t handler)
{
	lte_evt_handler = handler;

	return 0;
}

static int lte_lc_offline_custom(void)
{
	return 0;
}

static int lte_lc_func_mode_set_custom(enum lte_lc_func_mode mode)
{
	ARG_UNUSED(mode);

	return 0;
}

static int lte_lc_system_mode_get_custom(enum lte_lc_system_mode *mode,
	enum lte_lc_system_mode_preference *pref)
{
	ARG_UNUSED(pref);

	*mode = current_sys_mode;

	return 0;
}

static int lte_lc_system_mode_set_custom(enum lte_lc_system_mode mode,
	enum lte_lc_system_mode_preference pref)
{
	ARG_UNUSED(pref);

	current_sys_mode = mode;

	return 0;
}

static void ntn_register_handler_custom(ntn_evt_handler_t handler)
{
	ntn_evt_handler_cb = handler;
}

/* Event injection helpers */

static void inject_pdn_activated(void)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_PDN,
		.pdn.type = LTE_LC_EVT_PDN_ACTIVATED,
		.pdn.cid = 0,
	};

	lte_evt_handler(&evt);
}

static void inject_pdn_deactivated(void)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_PDN,
		.pdn.type = LTE_LC_EVT_PDN_DEACTIVATED,
		.pdn.cid = 0,
	};

	lte_evt_handler(&evt);
}

static void inject_search_done(void)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_MODEM_EVENT,
		.modem_evt.type = LTE_LC_MODEM_EVT_SEARCH_DONE,
	};

	lte_evt_handler(&evt);
}

#if defined(CONFIG_APP_NETWORK_NTN_TN_SEARCH_USE_LIGHT_SEARCH)
static void inject_light_search_done(void)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_MODEM_EVENT,
		.modem_evt.type = LTE_LC_MODEM_EVT_LIGHT_SEARCH_DONE,
	};

	lte_evt_handler(&evt);
}
#endif

static void inject_psm_update(void)
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

static void inject_edrx_update(void)
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

static void inject_uicc_failure(void)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_NW_REG_STATUS,
		.nw_reg_status = LTE_LC_NW_REG_UICC_FAIL,
	};

	lte_evt_handler(&evt);
}

static void inject_ntn_location_request(bool requested, uint32_t accuracy)
{
	struct ntn_evt evt = {
		.type = NTN_EVT_LOCATION_REQUEST,
		.location_request = {
			.requested = requested,
			.accuracy = accuracy,
		},
	};

	ntn_evt_handler_cb(&evt);
}

static void publish_network_msg(enum network_msg_type type)
{
	int err;
	struct network_msg msg = { .type = type };

	err = zbus_chan_pub(&network_chan, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
}

static void publish_connect_ntn(double lat, double lon, float alt)
{
	int err;
	struct network_msg msg = {
		.type = NETWORK_CONNECT_NTN,
		.prev_location = {
			.latitude = lat,
			.longitude = lon,
			.altitude = alt,
		},
	};

	err = zbus_chan_pub(&network_chan, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
}

/* Message verification helpers */

static void wait_for_network_msg(struct network_msg *msg,
				 enum network_msg_type expected)
{
	const struct zbus_channel *chan;
	int err;
	uint64_t deadline = k_uptime_get() + 2000;

	while (k_uptime_get() < deadline) {
		err = zbus_sub_wait_msg(&test_subscriber, &chan, msg,
					K_MSEC(1000));
		if (err == -ENOMSG) {
			LOG_ERR("No message received");
			TEST_FAIL();
		} else if (err) {
			LOG_ERR("zbus_sub_wait: %d", err);
			TEST_FAIL();
		}

		if (chan != &network_chan) {
			continue;
		}

		if (msg->type == expected) {
			return;
		}
	}

	LOG_ERR("Timeout waiting for msg type %d", expected);
	TEST_FAIL();
}

static void drive_to_idle(void)
{
	publish_network_msg(NETWORK_DISCONNECT);
	k_sleep(K_MSEC(100));
	inject_pdn_deactivated();
	k_sleep(K_MSEC(100));
}

static void drive_to_connected_ntn(void)
{
	struct network_msg rx;

	drive_to_idle();

	publish_network_msg(NETWORK_CONNECT);
	k_sleep(K_MSEC(100));

	inject_search_done();
	k_sleep(K_MSEC(100));

	publish_connect_ntn(NTN_LAT, NTN_LON, NTN_ALT);
	k_sleep(K_MSEC(100));

	inject_pdn_activated();

	wait_for_network_msg(&rx, NETWORK_CONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_CONNECTION_NTN, rx.connection_type);
}

static void drive_to_connected_tn(void)
{
	struct network_msg rx;

	drive_to_idle();

	publish_network_msg(NETWORK_CONNECT);
	k_sleep(K_MSEC(100));

	inject_pdn_activated();

	wait_for_network_msg(&rx, NETWORK_CONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_CONNECTION_TN, rx.connection_type);
}

/* setUp / tearDown */

void setUp(void)
{
	RESET_FAKE(nrf_modem_lib_init);
	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(task_wdt_add);
	RESET_FAKE(lte_lc_register_handler);
	RESET_FAKE(lte_lc_system_mode_get);
	RESET_FAKE(lte_lc_system_mode_set);
	RESET_FAKE(lte_lc_offline);
	RESET_FAKE(lte_lc_connect_async);
	RESET_FAKE(lte_lc_pdn_default_ctx_events_enable);
	RESET_FAKE(lte_lc_func_mode_set);
	RESET_FAKE(ntn_register_handler);
	RESET_FAKE(ntn_location_set);
	RESET_FAKE(ntn_location_invalidate);

	lte_lc_register_handler_fake.custom_fake = lte_lc_register_handler_custom;
	lte_lc_pdn_default_ctx_events_enable_fake.custom_fake =
		lte_lc_pdn_default_ctx_events_enable_custom;
	lte_lc_connect_async_fake.custom_fake = lte_lc_connect_async_custom;
	lte_lc_offline_fake.custom_fake = lte_lc_offline_custom;
	lte_lc_func_mode_set_fake.custom_fake = lte_lc_func_mode_set_custom;
	lte_lc_system_mode_get_fake.custom_fake = lte_lc_system_mode_get_custom;
	lte_lc_system_mode_set_fake.custom_fake = lte_lc_system_mode_set_custom;
	ntn_register_handler_fake.custom_fake = ntn_register_handler_custom;

	current_sys_mode = LTE_LC_SYSTEM_MODE_LTEM_NBIOT;

	k_sleep(K_MSEC(500));
}

void tearDown(void)
{
	const struct zbus_channel *chan;
	struct network_msg msg;

	while (zbus_sub_wait_msg(&test_subscriber, &chan, &msg,
				 K_MSEC(500)) == 0) {
		LOG_INF("tearDown: drained msg type %d", msg.type);
	}
}

/* Test cases */

void test_tn_connect(void)
{
	struct network_msg rx;

	drive_to_idle();

	publish_network_msg(NETWORK_CONNECT);
	k_sleep(K_MSEC(100));

	inject_pdn_activated();

	wait_for_network_msg(&rx, NETWORK_CONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_CONNECTED, rx.type);
	TEST_ASSERT_EQUAL(NETWORK_CONNECTION_TN, rx.connection_type);
}

void test_tn_search_fails_goes_to_idle(void)
{
	int connect_count_before;

	drive_to_idle();

	publish_network_msg(NETWORK_CONNECT);
	k_sleep(K_MSEC(100));

	inject_search_done();
	k_sleep(K_MSEC(100));

	connect_count_before = lte_lc_connect_async_fake.call_count;

	publish_network_msg(NETWORK_CONNECT);
	k_sleep(K_MSEC(100));

	TEST_ASSERT_GREATER_THAN(connect_count_before,
				 lte_lc_connect_async_fake.call_count);
}

void test_connect_ntn_from_idle(void)
{
	struct network_msg rx;

	drive_to_idle();

	publish_network_msg(NETWORK_CONNECT);
	k_sleep(K_MSEC(100));

	inject_search_done();
	k_sleep(K_MSEC(100));

	RESET_FAKE(ntn_location_set);
	ntn_location_set_fake.return_val = 0;

	publish_connect_ntn(NTN_LAT, NTN_LON, NTN_ALT);
	k_sleep(K_MSEC(100));

	TEST_ASSERT_GREATER_OR_EQUAL(1, ntn_location_set_fake.call_count);
	TEST_ASSERT_DOUBLE_WITHIN(0.001, NTN_LAT,
				  ntn_location_set_fake.arg0_val);
	TEST_ASSERT_DOUBLE_WITHIN(0.001, NTN_LON,
				  ntn_location_set_fake.arg1_val);
	TEST_ASSERT_FLOAT_WITHIN(0.1f, NTN_ALT,
				 ntn_location_set_fake.arg2_val);
	TEST_ASSERT_EQUAL(LTE_LC_SYSTEM_MODE_NTN_NBIOT, current_sys_mode);

	inject_pdn_activated();

	wait_for_network_msg(&rx, NETWORK_CONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_CONNECTION_NTN, rx.connection_type);
}

void test_pdn_activated_while_idle_after_tn_fail(void)
{
	struct network_msg rx;

	drive_to_idle();

	publish_network_msg(NETWORK_CONNECT);
	k_sleep(K_MSEC(100));

	inject_search_done();
	k_sleep(K_MSEC(100));

	inject_pdn_activated();

	wait_for_network_msg(&rx, NETWORK_CONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_CONNECTION_TN, rx.connection_type);
}

void test_tn_search_timeout_goes_to_idle(void)
{
	int connect_count_before;

	drive_to_idle();

	publish_network_msg(NETWORK_CONNECT);
	k_sleep(K_MSEC(100));

	k_sleep(K_SECONDS(CONFIG_APP_NETWORK_NTN_TN_SEARCH_TIMEOUT_SECONDS + 2));

	connect_count_before = lte_lc_connect_async_fake.call_count;

	publish_network_msg(NETWORK_CONNECT);
	k_sleep(K_MSEC(100));

	TEST_ASSERT_GREATER_THAN(connect_count_before,
				 lte_lc_connect_async_fake.call_count);
}

void test_ntn_search_fails_returns_to_idle(void)
{
	int connect_count_before;

	drive_to_connected_ntn();

	publish_network_msg(NETWORK_DISCONNECT);
	k_sleep(K_MSEC(100));
	inject_pdn_deactivated();
	k_sleep(K_MSEC(100));

	publish_network_msg(NETWORK_CONNECT);
	k_sleep(K_MSEC(100));

	inject_search_done();
	k_sleep(K_MSEC(100));

	publish_connect_ntn(NTN_LAT, NTN_LON, NTN_ALT);
	k_sleep(K_MSEC(100));

	TEST_ASSERT_EQUAL(LTE_LC_SYSTEM_MODE_NTN_NBIOT, current_sys_mode);

	inject_search_done();
	k_sleep(K_MSEC(100));

	connect_count_before = lte_lc_connect_async_fake.call_count;

	publish_network_msg(NETWORK_CONNECT);
	k_sleep(K_MSEC(100));

	TEST_ASSERT_GREATER_THAN(connect_count_before,
				 lte_lc_connect_async_fake.call_count);
}

void test_disconnect_from_tn(void)
{
	struct network_msg rx;

	drive_to_connected_tn();

	publish_network_msg(NETWORK_DISCONNECT);

	inject_pdn_deactivated();

	wait_for_network_msg(&rx, NETWORK_DISCONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_DISCONNECTED, rx.type);
}

void test_disconnect_from_ntn(void)
{
	struct network_msg rx;
	int mode_set_before;

	drive_to_connected_ntn();

	mode_set_before = lte_lc_system_mode_set_fake.call_count;

	publish_network_msg(NETWORK_DISCONNECT);
	k_sleep(K_MSEC(100));

	inject_pdn_deactivated();

	wait_for_network_msg(&rx, NETWORK_DISCONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_DISCONNECTED, rx.type);

	TEST_ASSERT_GREATER_THAN(mode_set_before,
				 lte_lc_system_mode_set_fake.call_count);
}

void test_disconnect_during_tn_search(void)
{
	int connect_count_before;

	drive_to_idle();

	publish_network_msg(NETWORK_CONNECT);
	k_sleep(K_MSEC(100));

	publish_network_msg(NETWORK_DISCONNECT);
	k_sleep(K_MSEC(100));

	TEST_ASSERT_GREATER_OR_EQUAL(1, lte_lc_offline_fake.call_count);

	connect_count_before = lte_lc_connect_async_fake.call_count;

	publish_network_msg(NETWORK_CONNECT);
	k_sleep(K_MSEC(100));

	TEST_ASSERT_GREATER_THAN(connect_count_before,
				 lte_lc_connect_async_fake.call_count);
}

void test_ntn_location_request_resends_cached(void)
{
	drive_to_connected_ntn();

	RESET_FAKE(ntn_location_set);
	ntn_location_set_fake.return_val = 0;

	inject_ntn_location_request(true, 200);
	k_sleep(K_MSEC(200));

	TEST_ASSERT_GREATER_OR_EQUAL(1, ntn_location_set_fake.call_count);
	TEST_ASSERT_DOUBLE_WITHIN(0.001, NTN_LAT,
				  ntn_location_set_fake.arg0_val);
	TEST_ASSERT_DOUBLE_WITHIN(0.001, NTN_LON,
				  ntn_location_set_fake.arg1_val);
	TEST_ASSERT_FLOAT_WITHIN(0.1f, NTN_ALT,
				 ntn_location_set_fake.arg2_val);
}

void test_psm_params_update(void)
{
	struct network_msg rx;

	inject_psm_update();

	wait_for_network_msg(&rx, NETWORK_PSM_PARAMS);
	TEST_ASSERT_EQUAL(NETWORK_PSM_PARAMS, rx.type);
	TEST_ASSERT_EQUAL(FAKE_PSM_TAU, rx.psm_cfg.tau);
	TEST_ASSERT_EQUAL(FAKE_PSM_ACTIVE_TIME, rx.psm_cfg.active_time);
}

void test_edrx_params_update(void)
{
	struct network_msg rx;

	inject_edrx_update();

	wait_for_network_msg(&rx, NETWORK_EDRX_PARAMS);
	TEST_ASSERT_EQUAL(NETWORK_EDRX_PARAMS, rx.type);
	TEST_ASSERT_EQUAL(LTE_LC_LTE_MODE_LTEM, rx.edrx_cfg.mode);
	TEST_ASSERT_EQUAL(FAKE_EDRX_VALUE, rx.edrx_cfg.edrx);
	TEST_ASSERT_EQUAL(FAKE_EDRX_PTW, rx.edrx_cfg.ptw);
}

void test_uicc_failure(void)
{
	struct network_msg rx;

	inject_uicc_failure();

	wait_for_network_msg(&rx, NETWORK_UICC_FAILURE);
	TEST_ASSERT_EQUAL(NETWORK_UICC_FAILURE, rx.type);
}

void test_system_mode_request(void)
{
	struct network_msg rx;

	publish_network_msg(NETWORK_SYSTEM_MODE_REQUEST);

	wait_for_network_msg(&rx, NETWORK_SYSTEM_MODE_RESPONSE);
	TEST_ASSERT_EQUAL(NETWORK_SYSTEM_MODE_RESPONSE, rx.type);
	TEST_ASSERT_EQUAL(current_sys_mode, rx.system_mode);
}

extern int unity_main(void);

int main(void)
{
	(void)unity_main();

	return 0;
}
