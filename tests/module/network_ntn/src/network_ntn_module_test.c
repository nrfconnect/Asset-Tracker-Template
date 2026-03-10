/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <unity.h>
#include <zephyr/fff.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
#include <modem/modem_info.h>
#include <modem/ntn.h>

#include "app_common.h"
#include "network_ntn.h"

DEFINE_FFF_GLOBALS;

#define FAKE_TIME_MS			1723099642000
#define FAKE_PSM_TAU			3600
#define FAKE_PSM_ACTIVE_TIME		16
#define FAKE_EDRX_VALUE			163.84f
#define FAKE_EDRX_PTW			1.28f
#define FAKE_SYSTEM_MODE_DEFAULT	LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS

#define FAKE_LATITUDE			63.4195
#define FAKE_LONGITUDE			10.4020
#define FAKE_ALTITUDE			50.0f

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
FAKE_VALUE_FUNC(int, lte_lc_pdn_default_ctx_events_enable);
FAKE_VALUE_FUNC(int, lte_lc_func_mode_set, enum lte_lc_func_mode);
FAKE_VALUE_FUNC(int, lte_lc_func_mode_get, enum lte_lc_func_mode *);
FAKE_VOID_FUNC(ntn_register_handler, ntn_evt_handler_t);
FAKE_VALUE_FUNC(int, ntn_location_set, double, double, float, uint32_t);
FAKE_VALUE_FUNC(int, lte_lc_cellular_profile_configure, struct lte_lc_cellular_profile *);

/* Test subscriber on the public channel */
ZBUS_MSG_SUBSCRIBER_DEFINE(test_subscriber);
ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, test_subscriber, 0);

/* Use of private channel is a temporary workaround to allow testing until SGP4 is implemented. */
ZBUS_CHAN_DECLARE(PRIV_NETWORK_NTN_CHAN);

enum priv_network_ntn_msg {
	PERIODIC_TN_SEARCH,
	NTN_LEO_SATELLITE_PASS_UPCOMING,
	NTN_SEARCH_GEO_START,
	NTN_WAIT_FOR_SATELLITE_PASS,
	NTN_LOCATION_NEEDED,
};

static lte_lc_evt_handler_t lte_evt_handler;
static ntn_evt_handler_t ntn_evt_handler;
static enum lte_lc_system_mode current_fake_system_mode = FAKE_SYSTEM_MODE_DEFAULT;
static enum lte_lc_pdn_evt_type previous_fake_pdn_type;
static enum lte_lc_func_mode current_fake_functional_mode = LTE_LC_FUNC_MODE_OFFLINE;

/* Custom fakes */

static int date_time_now_custom_fake(int64_t *time)
{
	*time = FAKE_TIME_MS;

	return 0;
}

static void lte_lc_register_handler_custom_fake(lte_lc_evt_handler_t handler)
{
	lte_evt_handler = handler;
}

static void ntn_register_handler_custom_fake(ntn_evt_handler_t handler)
{
	ntn_evt_handler = handler;
}

static int lte_lc_pdn_default_ctx_events_enable_custom_fake(void)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_PDN,
		.pdn = {
			.type = LTE_LC_EVT_PDN_DEACTIVATED,
			.cid = 0,
		},
	};

	if (lte_evt_handler) {
		lte_evt_handler(&evt);
	}

	return 0;
}

static int lte_lc_func_mode_set_custom_fake(enum lte_lc_func_mode mode)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_PDN,
		.pdn = {
			.cid = 0,
		},
	};
	bool pdn_evt_changed = false;

	if (mode == LTE_LC_FUNC_MODE_ACTIVATE_LTE) {
		if (previous_fake_pdn_type != LTE_LC_EVT_PDN_ACTIVATED) {
			evt.pdn.type = LTE_LC_EVT_PDN_ACTIVATED;
			previous_fake_pdn_type = LTE_LC_EVT_PDN_ACTIVATED;
			pdn_evt_changed = true;
		}
	} else {
		if (previous_fake_pdn_type != LTE_LC_EVT_PDN_DEACTIVATED) {
			evt.pdn.type = LTE_LC_EVT_PDN_DEACTIVATED;
			previous_fake_pdn_type = LTE_LC_EVT_PDN_DEACTIVATED;
			pdn_evt_changed = true;
		}
	}

	current_fake_functional_mode = mode;

	if (lte_evt_handler && pdn_evt_changed) {
		lte_evt_handler(&evt);
	}

	return 0;
}

static int lte_lc_func_mode_set_no_network_custom_fake(enum lte_lc_func_mode mode)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_MODEM_EVENT,
		.modem_evt = {
			.type = LTE_LC_MODEM_EVT_SEARCH_DONE,
		},
	};
	bool send_event = false;

	if (mode == LTE_LC_FUNC_MODE_ACTIVATE_LTE) {
		evt.modem_evt.type = LTE_LC_MODEM_EVT_SEARCH_DONE;
		send_event = true;
	} else {
		if (previous_fake_pdn_type != LTE_LC_EVT_PDN_DEACTIVATED) {
			evt.pdn.type = LTE_LC_EVT_PDN_DEACTIVATED;
			previous_fake_pdn_type = LTE_LC_EVT_PDN_DEACTIVATED;
			send_event = true;
		}
	}

	current_fake_functional_mode = mode;

	if (lte_evt_handler && send_event) {
		lte_evt_handler(&evt);
	}

	return 0;
}

static int lte_lc_func_mode_get_custom_fake(enum lte_lc_func_mode *mode)
{
	*mode = current_fake_functional_mode;

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

static int date_time_now_far_future_fake(int64_t *time)
{
	/* Return a large negative value so that leo_satellite_search_timer_start()
	 * computes ~11 days.
	 * This prevents the LEO timer from firing during the test, allowing us to
	 * stay in WAITING_FOR_LEO long enough to test PERIODIC_TN_SEARCH.
	 */
	*time = -1000000000000LL;

	return 0;
}

/* Helper functions */

static void publish_network_msg(enum network_msg_type type)
{
	int err;
	struct network_msg msg = { .type = type };

	err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Allow the module time to process the message */
	k_sleep(K_MSEC(100));
}

static void publish_network_location_data(double lat, double lon, float alt)
{
	int err;
	struct network_msg msg = {
		.type = NETWORK_LOCATION_DATA,
		.location = {
			.latitude = lat,
			.longitude = lon,
			.altitude = alt,
		},
		.timestamp = FAKE_TIME_MS,
	};

	err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Allow the module time to process the message */
	k_sleep(K_MSEC(100));
}

static void publish_priv_chan_msg(enum priv_network_ntn_msg msg)
{
	int err;

	err = zbus_chan_pub(&PRIV_NETWORK_NTN_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Allow the module time to process the message */
	k_sleep(K_MSEC(100));
}
static void wait_for_msg_of_type(struct network_msg *msg, enum network_msg_type expected_type)
{
	int err;
	const struct zbus_channel *chan;

	while (true) {
		err = zbus_sub_wait_msg(&test_subscriber, &chan, msg, K_SECONDS(10));
		if (err == -ENOMSG) {
			TEST_FAIL();
		} else if (err) {
			SEND_FATAL_ERROR();

			return;
		}

		if (chan != &NETWORK_CHAN) {
			TEST_FAIL();
		}

		if (msg->type == expected_type) {
			return;
		}
	}
}

/* setUp / tearDown */

void setUp(void)
{
	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(task_wdt_add);
	RESET_FAKE(date_time_now);
	RESET_FAKE(lte_lc_conn_eval_params_get);
	RESET_FAKE(lte_lc_pdn_default_ctx_events_enable);
	RESET_FAKE(lte_lc_modem_events_enable);
	RESET_FAKE(lte_lc_system_mode_get);
	RESET_FAKE(lte_lc_system_mode_set);
	RESET_FAKE(lte_lc_register_handler);
	RESET_FAKE(lte_lc_func_mode_set);
	RESET_FAKE(lte_lc_cellular_profile_configure);
	RESET_FAKE(nrf_modem_lib_init);
	RESET_FAKE(ntn_register_handler);
	RESET_FAKE(ntn_location_set);
	RESET_FAKE(lte_lc_func_mode_get);

	date_time_now_fake.custom_fake = date_time_now_custom_fake;
	lte_lc_register_handler_fake.custom_fake = lte_lc_register_handler_custom_fake;
	ntn_register_handler_fake.custom_fake = ntn_register_handler_custom_fake;
	lte_lc_system_mode_get_fake.custom_fake = lte_lc_system_mode_get_custom_fake;
	lte_lc_system_mode_set_fake.custom_fake = lte_lc_system_mode_set_custom_fake;
	lte_lc_func_mode_set_fake.custom_fake = lte_lc_func_mode_set_custom_fake;
	lte_lc_pdn_default_ctx_events_enable_fake.custom_fake =
		lte_lc_pdn_default_ctx_events_enable_custom_fake;
	lte_lc_func_mode_get_fake.custom_fake = lte_lc_func_mode_get_custom_fake;

	previous_fake_pdn_type = LTE_LC_EVT_PDN_ESM_ERROR;
	current_fake_functional_mode = LTE_LC_FUNC_MODE_OFFLINE;

	k_sleep(K_MSEC(500));
}

void tearDown(void)
{
	const struct zbus_channel *chan;
	static struct network_msg msg;

	/* Drain the test subscriber queue of any messages from previous tests */
	while (zbus_sub_wait_msg(&test_subscriber, &chan, &msg, K_MSEC(1000)) == 0) {
	}
}

/* Test cases */

/* This is the only test case that should be sensitive to the order of the tests.
 * The intention is to verify that the initial state is DISCONNECTED.
 */
void test_initial_disconnected(void)
{
	struct network_msg msg = { 0 };

	wait_for_msg_of_type(&msg, NETWORK_DISCONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_DISCONNECTED, msg.type);
}

void test_psm_params_update(void)
{
	struct network_msg msg = { 0 };
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_PSM_UPDATE,
		.psm_cfg = {
			.tau = FAKE_PSM_TAU,
			.active_time = FAKE_PSM_ACTIVE_TIME,
		},
	};

	lte_evt_handler(&evt);

	wait_for_msg_of_type(&msg, NETWORK_PSM_PARAMS);
	TEST_ASSERT_EQUAL(NETWORK_PSM_PARAMS, msg.type);

	TEST_ASSERT_EQUAL(FAKE_PSM_TAU, msg.psm_cfg.tau);
	TEST_ASSERT_EQUAL(FAKE_PSM_ACTIVE_TIME, msg.psm_cfg.active_time);
}

void test_edrx_params_forwarded(void)
{
	struct network_msg msg = { 0 };
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_EDRX_UPDATE,
		.edrx_cfg = {
			.mode = LTE_LC_LTE_MODE_LTEM,
			.edrx = FAKE_EDRX_VALUE,
			.ptw = FAKE_EDRX_PTW,
		},
	};

	lte_evt_handler(&evt);

	wait_for_msg_of_type(&msg, NETWORK_EDRX_PARAMS);

	TEST_ASSERT_EQUAL(NETWORK_EDRX_PARAMS, msg.type);
	TEST_ASSERT_EQUAL(LTE_LC_LTE_MODE_LTEM, msg.edrx_cfg.mode);
	TEST_ASSERT_EQUAL(FAKE_EDRX_VALUE, msg.edrx_cfg.edrx);
	TEST_ASSERT_EQUAL(FAKE_EDRX_PTW, msg.edrx_cfg.ptw);
}

void test_uicc_failure(void)
{
	struct network_msg msg = { 0 };
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_NW_REG_STATUS,
		.nw_reg_status = LTE_LC_NW_REG_UICC_FAIL,
	};
	lte_evt_handler(&evt);

	wait_for_msg_of_type(&msg, NETWORK_UICC_FAILURE);
	TEST_ASSERT_EQUAL(NETWORK_UICC_FAILURE, msg.type);
}

void test_system_mode_request(void)
{
	struct network_msg msg = { 0 };

	publish_network_msg(NETWORK_SYSTEM_MODE_REQUEST);

	wait_for_msg_of_type(&msg, NETWORK_SYSTEM_MODE_RESPONSE);
	TEST_ASSERT_EQUAL(NETWORK_SYSTEM_MODE_RESPONSE, msg.type);
	TEST_ASSERT_EQUAL(current_fake_system_mode, msg.system_mode);
}

void test_connect_tn(void)
{
	struct network_msg msg = { 0 };

	publish_network_msg(NETWORK_DISCONNECT);
	publish_network_msg(NETWORK_CONNECT_TN);

	wait_for_msg_of_type(&msg, NETWORK_CONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_CONNECTED, msg.type);
}

void test_disconnect_from_connected(void)
{
	struct network_msg msg = { 0 };

	publish_network_msg(NETWORK_DISCONNECT);
	publish_network_msg(NETWORK_CONNECT_TN);

	wait_for_msg_of_type(&msg, NETWORK_CONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_CONNECTED, msg.type);

	publish_network_msg(NETWORK_DISCONNECT);

	wait_for_msg_of_type(&msg, NETWORK_DISCONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_DISCONNECTED, msg.type);

	TEST_ASSERT_GREATER_OR_EQUAL(1, lte_lc_func_mode_set_fake.call_count);
}

void test_connect_ntn_sets_ntn_system_mode(void)
{
	publish_network_msg(NETWORK_DISCONNECT);

	TEST_ASSERT_EQUAL(0, lte_lc_system_mode_set_fake.call_count);

	publish_network_msg(NETWORK_CONNECT_NTN);

	TEST_ASSERT_GREATER_OR_EQUAL(1, lte_lc_system_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_SYSTEM_MODE_NTN_NBIOT, lte_lc_system_mode_set_fake.arg0_val);
}

void test_ntn_location_needed_publishes_on_network_chan(void)
{
	struct network_msg msg = { 0 };

	publish_network_msg(NETWORK_DISCONNECT);
	publish_network_msg(NETWORK_CONNECT_NTN);

	publish_network_msg(NETWORK_LOCATION_NEEDED);

	wait_for_msg_of_type(&msg, NETWORK_LOCATION_NEEDED);
	TEST_ASSERT_EQUAL(NETWORK_LOCATION_NEEDED, msg.type);
}

void test_ntn_location_data_calls_ntn_location_set(void)
{
	publish_network_msg(NETWORK_DISCONNECT);

	lte_lc_func_mode_set_fake.custom_fake = lte_lc_func_mode_set_no_network_custom_fake;

	publish_network_msg(NETWORK_CONNECT_NTN);

	TEST_ASSERT_EQUAL(0, ntn_location_set_fake.call_count);


	/* Send NETWORK_LOCATION_DATA */
	publish_network_location_data(FAKE_LATITUDE, FAKE_LONGITUDE, FAKE_ALTITUDE);

	TEST_ASSERT_EQUAL(1, ntn_location_set_fake.call_count);
	TEST_ASSERT_FLOAT_WITHIN(0.1f, (float)FAKE_LATITUDE,
				 (float)ntn_location_set_fake.arg0_val);
	TEST_ASSERT_FLOAT_WITHIN(0.1f, (float)FAKE_LONGITUDE,
				 (float)ntn_location_set_fake.arg1_val);
	TEST_ASSERT_FLOAT_WITHIN(0.1f, FAKE_ALTITUDE, ntn_location_set_fake.arg2_val);
}

void test_ntn_search_no_suitable_cell_returns_to_idle_to_connect_tn(void)
{
	struct network_msg msg = { 0 };

	publish_network_msg(NETWORK_DISCONNECT);

	TEST_ASSERT_EQUAL(0, lte_lc_system_mode_set_fake.call_count);

	lte_lc_func_mode_set_fake.custom_fake = lte_lc_func_mode_set_no_network_custom_fake;

	publish_network_msg(NETWORK_CONNECT_NTN);

	TEST_ASSERT_EQUAL(1, lte_lc_system_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_SYSTEM_MODE_NTN_NBIOT, lte_lc_system_mode_set_fake.arg0_val);

	/* Verify that we are in the NTN_SEARCH_PREPARE state */
	publish_network_msg(NETWORK_LOCATION_NEEDED);

	wait_for_msg_of_type(&msg, NETWORK_LOCATION_NEEDED);
	TEST_ASSERT_EQUAL(NETWORK_LOCATION_NEEDED, msg.type);

	/* Going back to IDLE */
	publish_network_msg(NETWORK_NTN_NO_SUITABLE_CELL);

	lte_lc_func_mode_set_fake.custom_fake = lte_lc_func_mode_set_custom_fake;

	/* NETWORK_CONNECT_TN is only handled in the IDLE state */
	publish_network_msg(NETWORK_CONNECT_TN);

	TEST_ASSERT_EQUAL(2, lte_lc_system_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS, lte_lc_system_mode_set_fake.arg0_val);

	wait_for_msg_of_type(&msg, NETWORK_CONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_CONNECTED, msg.type);
}

void test_leo_pass_upcoming_triggers_leo_search(void)
{
	publish_network_msg(NETWORK_DISCONNECT);

	TEST_ASSERT_EQUAL(1, lte_lc_func_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG, lte_lc_func_mode_set_fake.arg0_val);

	publish_priv_chan_msg(NTN_LEO_SATELLITE_PASS_UPCOMING);

	TEST_ASSERT_EQUAL(1, lte_lc_system_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_SYSTEM_MODE_NTN_NBIOT, lte_lc_system_mode_set_fake.arg0_val);

	/* Expected that both disconnect and connect calls are made. The important thing is that the
	 * last call is ACTIVATE_LTE.
	 */
	TEST_ASSERT_EQUAL(4, lte_lc_func_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG,
			  lte_lc_func_mode_set_fake.arg0_history[1]);
	TEST_ASSERT_EQUAL(LTE_LC_FUNC_MODE_ACTIVATE_LTE, lte_lc_func_mode_set_fake.arg0_val);
}

void test_wait_for_leo_periodic_tn_search(void)
{
	publish_network_msg(NETWORK_DISCONNECT);

	/* Do not connect automatically. Needed to get reapeated periodic TN searches. */
	lte_lc_func_mode_set_fake.custom_fake = lte_lc_func_mode_set_no_network_custom_fake;

	publish_network_msg(NETWORK_CONNECT_NTN);

	/* Use a date_time_now fake that makes the LEO timer delay very large,
	 * preventing NTN_LEO_SATELLITE_PASS_UPCOMING from firing immediately.
	 */
	date_time_now_fake.custom_fake = date_time_now_far_future_fake;

	publish_priv_chan_msg(NTN_WAIT_FOR_SATELLITE_PASS);

	/* Test that we get 3 periodic TN searches */
	for (int i = 0; i < 3; i++) {
		/* Wait for CONFIG_APP_NETWORK_NTN_PERIODIC_TN_SEARCH_INTERVAL_SECONDS seconds */
		k_sleep(K_SECONDS(CONFIG_APP_NETWORK_NTN_PERIODIC_TN_SEARCH_INTERVAL_SECONDS));

		TEST_ASSERT_EQUAL(2 + i, lte_lc_system_mode_set_fake.call_count);
		TEST_ASSERT_EQUAL(LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS,
				  lte_lc_system_mode_set_fake.arg0_val);
		TEST_ASSERT_EQUAL(3 + i, lte_lc_func_mode_set_fake.call_count);
		TEST_ASSERT_EQUAL(LTE_LC_FUNC_MODE_ACTIVATE_LTE,
				  lte_lc_func_mode_set_fake.arg0_val);
	}
}

void test_location_failed_in_prepare(void)
{
	publish_network_msg(NETWORK_DISCONNECT);

	TEST_ASSERT_EQUAL(1, lte_lc_func_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG, lte_lc_func_mode_set_fake.arg0_val);

	publish_network_msg(NETWORK_CONNECT_NTN);
	publish_network_msg(NETWORK_LOCATION_FAILED);

	TEST_ASSERT_EQUAL(2, lte_lc_func_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_FUNC_MODE_ACTIVATE_LTE, lte_lc_func_mode_set_fake.arg0_val);
}

void test_no_suitable_cell_returns_to_idle(void)
{
	struct network_msg msg = { 0 };
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_NW_REG_STATUS,
		.nw_reg_status = LTE_LC_NW_REG_NO_SUITABLE_CELL,
	};

	publish_network_msg(NETWORK_DISCONNECT);

	lte_lc_func_mode_set_fake.custom_fake = lte_lc_func_mode_set_no_network_custom_fake;

	publish_network_msg(NETWORK_CONNECT_NTN);

	TEST_ASSERT_EQUAL(1, lte_lc_system_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_SYSTEM_MODE_NTN_NBIOT, lte_lc_system_mode_set_fake.arg0_val);

	TEST_ASSERT_EQUAL(2, lte_lc_func_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_FUNC_MODE_ACTIVATE_LTE, lte_lc_func_mode_set_fake.arg0_val);

	lte_evt_handler(&evt);

	wait_for_msg_of_type(&msg, NETWORK_NTN_NO_SUITABLE_CELL);

	TEST_ASSERT_EQUAL(NETWORK_NTN_NO_SUITABLE_CELL, msg.type);

	/* Should have returned to IDLE -- verify by sending CONNECT_TN */
	publish_network_msg(NETWORK_CONNECT_TN);

	TEST_ASSERT_EQUAL(2, lte_lc_system_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS, lte_lc_system_mode_set_fake.arg0_val);

	TEST_ASSERT_EQUAL(4, lte_lc_func_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_FUNC_MODE_ACTIVATE_LTE, lte_lc_func_mode_set_fake.arg0_val);
}

void test_attach_rejected_forwarded(void)
{
	struct network_msg msg = { 0 };
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_NW_REG_STATUS,
		.nw_reg_status = LTE_LC_NW_REG_NOT_REGISTERED,
	};

	lte_evt_handler(&evt);

	wait_for_msg_of_type(&msg, NETWORK_ATTACH_REJECTED);
	TEST_ASSERT_EQUAL(NETWORK_ATTACH_REJECTED, msg.type);
}

void test_modem_reset_loop_forwarded(void)
{
	struct network_msg msg = { 0 };
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_MODEM_EVENT,
		.modem_evt = {
			.type = LTE_LC_MODEM_EVT_RESET_LOOP,
		},
	};

	lte_evt_handler(&evt);

	wait_for_msg_of_type(&msg, NETWORK_MODEM_RESET_LOOP);
	TEST_ASSERT_EQUAL(NETWORK_MODEM_RESET_LOOP, msg.type);
}

void test_light_search_done_forwarded(void)
{
	struct network_msg msg = { 0 };
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_MODEM_EVENT,
		.modem_evt = {
			.type = LTE_LC_MODEM_EVT_LIGHT_SEARCH_DONE,
		},
	};

	lte_evt_handler(&evt);

	wait_for_msg_of_type(&msg, NETWORK_LIGHT_SEARCH_DONE);
	TEST_ASSERT_EQUAL(NETWORK_LIGHT_SEARCH_DONE, msg.type);
}

void test_search_done_forwarded(void)
{
	struct network_msg msg = { 0 };
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_MODEM_EVENT,
		.modem_evt = {
			.type = LTE_LC_MODEM_EVT_SEARCH_DONE,
		},
	};

	lte_evt_handler(&evt);

	wait_for_msg_of_type(&msg, NETWORK_SEARCH_DONE);
	TEST_ASSERT_EQUAL(NETWORK_SEARCH_DONE, msg.type);
}

void test_pdn_network_detach_forwards_disconnected(void)
{
	struct network_msg msg = { 0 };
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_PDN,
		.pdn = {
			.type = LTE_LC_EVT_PDN_NETWORK_DETACH,
			.cid = 0,
		},
	};

	lte_evt_handler(&evt);

	wait_for_msg_of_type(&msg, NETWORK_DISCONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_DISCONNECTED, msg.type);
}

void test_pdn_suspended_forwards_disconnected(void)
{
	struct network_msg msg = { 0 };
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_PDN,
		.pdn = {
			.type = LTE_LC_EVT_PDN_SUSPENDED,
			.cid = 0,
		},
	};

	lte_evt_handler(&evt);

	wait_for_msg_of_type(&msg, NETWORK_DISCONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_DISCONNECTED, msg.type);
}

void test_pdn_resumed_forwards_connected(void)
{
	struct network_msg msg = { 0 };
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_PDN,
		.pdn = {
			.type = LTE_LC_EVT_PDN_RESUMED,
			.cid = 0,
		},
	};

	lte_evt_handler(&evt);

	wait_for_msg_of_type(&msg, NETWORK_CONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_CONNECTED, msg.type);
}

void test_system_mode_set_ltem_in_idle(void)
{
	publish_network_msg(NETWORK_DISCONNECT);
	publish_network_msg(NETWORK_SYSTEM_MODE_SET_LTEM);

	TEST_ASSERT_EQUAL(1, lte_lc_system_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_SYSTEM_MODE_LTEM_GPS, lte_lc_system_mode_set_fake.arg0_val);
}

void test_system_mode_set_nbiot_in_idle(void)
{
	publish_network_msg(NETWORK_DISCONNECT);
	publish_network_msg(NETWORK_SYSTEM_MODE_SET_NBIOT);

	TEST_ASSERT_EQUAL(1, lte_lc_system_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_SYSTEM_MODE_NBIOT_GPS, lte_lc_system_mode_set_fake.arg0_val);
}

void test_system_mode_set_ltem_nbiot_in_idle(void)
{
	publish_network_msg(NETWORK_DISCONNECT);
	publish_network_msg(NETWORK_SYSTEM_MODE_SET_LTEM_NBIOT);

	TEST_ASSERT_EQUAL(1, lte_lc_system_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS, lte_lc_system_mode_set_fake.arg0_val);
}

void test_search_stop_returns_to_idle(void)
{
	lte_lc_func_mode_set_fake.custom_fake = lte_lc_func_mode_set_no_network_custom_fake;

	publish_network_msg(NETWORK_DISCONNECT);
	publish_network_msg(NETWORK_CONNECT_TN);

	TEST_ASSERT_EQUAL(1, lte_lc_system_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS, lte_lc_system_mode_set_fake.arg0_val);

	TEST_ASSERT_EQUAL(1, lte_lc_func_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_FUNC_MODE_ACTIVATE_LTE, lte_lc_func_mode_set_fake.arg0_val);

	publish_network_msg(NETWORK_SEARCH_STOP);

	TEST_ASSERT_EQUAL(2, lte_lc_func_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG, lte_lc_func_mode_set_fake.arg0_val);

	publish_network_msg(NETWORK_CONNECT_TN);

	TEST_ASSERT_EQUAL(2, lte_lc_system_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS, lte_lc_system_mode_set_fake.arg0_val);

	TEST_ASSERT_EQUAL(3, lte_lc_func_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_FUNC_MODE_ACTIVATE_LTE, lte_lc_func_mode_set_fake.arg0_val);
}

void test_leo_search_to_connected(void)
{
	struct network_msg msg = { 0 };

	publish_network_msg(NETWORK_DISCONNECT);
	publish_network_msg(NETWORK_CONNECT_NTN);

	/* NTN_LEO_SATELLITE_PASS_UPCOMING in PREPARE transitions to LEO state.
	 * LEO entry calls network_connect() which fires PDN_ACTIVATED via the
	 * custom fake → NETWORK_CONNECTED is published.
	 */
	publish_priv_chan_msg(NTN_LEO_SATELLITE_PASS_UPCOMING);

	wait_for_msg_of_type(&msg, NETWORK_CONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_CONNECTED, msg.type);

	/* Verify we can cleanly disconnect */
	publish_network_msg(NETWORK_DISCONNECT);

	wait_for_msg_of_type(&msg, NETWORK_DISCONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_DISCONNECTED, msg.type);
}

void test_tn_failed_to_leo_search_to_connected(void)
{
	struct network_msg msg = { 0 };
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_MODEM_EVENT,
		.modem_evt = {
			.type = LTE_LC_MODEM_EVT_SEARCH_DONE,
		},
	};

	publish_network_msg(NETWORK_DISCONNECT);

	lte_lc_func_mode_set_fake.custom_fake = lte_lc_func_mode_set_no_network_custom_fake;

	publish_network_msg(NETWORK_CONNECT_TN);

	TEST_ASSERT_EQUAL(1, lte_lc_system_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS, lte_lc_system_mode_set_fake.arg0_val);

	TEST_ASSERT_EQUAL(1, lte_lc_func_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_FUNC_MODE_ACTIVATE_LTE, lte_lc_func_mode_set_fake.arg0_val);

	lte_evt_handler(&evt);

	wait_for_msg_of_type(&msg, NETWORK_SEARCH_DONE);
	TEST_ASSERT_EQUAL(NETWORK_SEARCH_DONE, msg.type);

	publish_network_msg(NETWORK_SEARCH_STOP);

	TEST_ASSERT_EQUAL(2, lte_lc_func_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG, lte_lc_func_mode_set_fake.arg0_val);

	publish_network_msg(NETWORK_CONNECT_NTN);

	TEST_ASSERT_EQUAL(2, lte_lc_system_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_SYSTEM_MODE_NTN_NBIOT, lte_lc_system_mode_set_fake.arg0_val);

	TEST_ASSERT_EQUAL(3, lte_lc_func_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_FUNC_MODE_ACTIVATE_LTE, lte_lc_func_mode_set_fake.arg0_val);

	lte_lc_func_mode_set_fake.custom_fake = lte_lc_func_mode_set_custom_fake;

	publish_priv_chan_msg(NTN_LEO_SATELLITE_PASS_UPCOMING);

	wait_for_msg_of_type(&msg, NETWORK_CONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_CONNECTED, msg.type);
}

void test_disconnect_while_idle_is_noop(void)
{
	publish_network_msg(NETWORK_DISCONNECT);

	RESET_FAKE(lte_lc_func_mode_set);

	publish_network_msg(NETWORK_DISCONNECT);

	TEST_ASSERT_EQUAL(0, lte_lc_func_mode_set_fake.call_count);

	/* Confirm still in IDLE by accepting CONNECT_TN */
	publish_network_msg(NETWORK_CONNECT_TN);

	TEST_ASSERT_EQUAL(1, lte_lc_func_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_FUNC_MODE_ACTIVATE_LTE, lte_lc_func_mode_set_fake.arg0_val);
}

/* This is required to be added to each test. That is because unity's
 * main may return nonzero, while zephyr's main currently must
 * return 0 in all cases (other values are reserved).
 */
extern int unity_main(void);

int main(void)
{
	(void)unity_main();

	return 0;
}
