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
FAKE_VALUE_FUNC(int, lte_lc_connect_async, lte_lc_evt_handler_t);
FAKE_VALUE_FUNC(int, lte_lc_pdn_default_ctx_events_enable);
FAKE_VALUE_FUNC(int, lte_lc_func_mode_set, enum lte_lc_func_mode);
FAKE_VOID_FUNC(ntn_register_handler, ntn_evt_handler_t);
FAKE_VALUE_FUNC(int, ntn_location_set, double, double, float, uint32_t);

/* Private channel enum mirrored from network_ntn.c for test access */
enum priv_network_ntn_msg {
	LEO_SATELLITE_PASS_UPCOMING,
	PERIODIC_TN_SEARCH,
	NTN_LEO_SATELLITE_PASS_UPCOMING,
	NTN_SEARCH_GEO_START,
	NTN_WAIT_FOR_SATELLITE_PASS,
	NTN_LOCATION_NEEDED,
};

ZBUS_CHAN_DECLARE(PRIV_NETWORK_NTN_CHAN);

/* Test subscriber on the public channel */
ZBUS_MSG_SUBSCRIBER_DEFINE(test_subscriber);
ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, test_subscriber, 0);

static lte_lc_evt_handler_t lte_evt_handler;
static ntn_evt_handler_t ntn_evt_handler;
static enum lte_lc_system_mode current_fake_system_mode = FAKE_SYSTEM_MODE_DEFAULT;

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

static int lte_lc_connect_async_custom_fake(lte_lc_evt_handler_t handler)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_PDN,
		.pdn = {
			.type = LTE_LC_EVT_PDN_ACTIVATED,
			.cid = 0,
		},
	};

	lte_evt_handler = handler;

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

	if (mode == LTE_LC_FUNC_MODE_ACTIVATE_LTE) {
		evt.pdn.type = LTE_LC_EVT_PDN_ACTIVATED;
	} else {
		evt.pdn.type = LTE_LC_EVT_PDN_DEACTIVATED;
	}

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
		err = zbus_sub_wait_msg(&test_subscriber, &chan, msg, K_MSEC(1000));
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
	RESET_FAKE(lte_lc_connect_async);
	RESET_FAKE(lte_lc_pdn_default_ctx_events_enable);
	RESET_FAKE(lte_lc_modem_events_enable);
	RESET_FAKE(lte_lc_system_mode_get);
	RESET_FAKE(lte_lc_system_mode_set);
	RESET_FAKE(lte_lc_register_handler);
	RESET_FAKE(lte_lc_func_mode_set);
	RESET_FAKE(nrf_modem_lib_init);
	RESET_FAKE(ntn_register_handler);
	RESET_FAKE(ntn_location_set);

	date_time_now_fake.custom_fake = date_time_now_custom_fake;
	lte_lc_register_handler_fake.custom_fake = lte_lc_register_handler_custom_fake;
	ntn_register_handler_fake.custom_fake = ntn_register_handler_custom_fake;
	lte_lc_system_mode_get_fake.custom_fake = lte_lc_system_mode_get_custom_fake;
	lte_lc_system_mode_set_fake.custom_fake = lte_lc_system_mode_set_custom_fake;
	lte_lc_connect_async_fake.custom_fake = lte_lc_connect_async_custom_fake;
	lte_lc_func_mode_set_fake.custom_fake = lte_lc_func_mode_set_custom_fake;
	lte_lc_pdn_default_ctx_events_enable_fake.custom_fake =
		lte_lc_pdn_default_ctx_events_enable_custom_fake;

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

void test_initial_disconnected(void)
{
	struct network_msg msg = { 0 };

	wait_for_msg_of_type(&msg, NETWORK_DISCONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_DISCONNECTED, msg.type);
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

	/* Don't connect automatically, we just want to see the sys mode set */
	lte_lc_connect_async_fake.custom_fake = NULL;
	lte_lc_connect_async_fake.return_val = 0;

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

	publish_priv_chan_msg(NTN_LOCATION_NEEDED);

	wait_for_msg_of_type(&msg, NETWORK_LOCATION_NEEDED);
	TEST_ASSERT_EQUAL(NETWORK_LOCATION_NEEDED, msg.type);
}

void test_ntn_location_data_calls_ntn_location_set(void)
{
	publish_network_msg(NETWORK_DISCONNECT);
	publish_network_msg(NETWORK_CONNECT_NTN);

	TEST_ASSERT_EQUAL(0, ntn_location_set_fake.call_count);

	/* Send location data to PREPARE state */
	publish_network_location_data(FAKE_LATITUDE, FAKE_LONGITUDE, FAKE_ALTITUDE);

	TEST_ASSERT_GREATER_OR_EQUAL(1, ntn_location_set_fake.call_count);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, (float)FAKE_LATITUDE,
				 (float)ntn_location_set_fake.arg0_val);
	TEST_ASSERT_FLOAT_WITHIN(0.001f, (float)FAKE_LONGITUDE,
				 (float)ntn_location_set_fake.arg1_val);
	TEST_ASSERT_FLOAT_WITHIN(0.1f, FAKE_ALTITUDE, ntn_location_set_fake.arg2_val);
}

void test_ntn_search_no_suitable_cell_returns_to_idle(void)
{
	struct network_msg msg;

	publish_network_msg(NETWORK_DISCONNECT);
	publish_network_msg(NETWORK_CONNECT_NTN);

	publish_network_msg(NETWORK_NTN_NO_SUITABLE_CELL);

	publish_network_msg(NETWORK_CONNECT_TN);

	wait_for_msg_of_type(&msg, NETWORK_CONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_CONNECTED, msg.type);
}

void test_leo_pass_upcoming_triggers_ntn_search(void)
{
	publish_network_msg(NETWORK_DISCONNECT);

	/* Don't auto-connect, we want to verify state transition */
	lte_lc_connect_async_fake.custom_fake = NULL;
	lte_lc_connect_async_fake.return_val = 0;

	RESET_FAKE(lte_lc_func_mode_set);
	lte_lc_func_mode_set_fake.return_val = 0;

	publish_priv_chan_msg(LEO_SATELLITE_PASS_UPCOMING);

	/* Should have called network_disconnect() */
	TEST_ASSERT_GREATER_OR_EQUAL(1, lte_lc_func_mode_set_fake.call_count);

	/* Should have set NB-IoT mode (entering NTN_SEARCH) */
	TEST_ASSERT_GREATER_OR_EQUAL(1, lte_lc_system_mode_set_fake.call_count);
}

static int date_time_now_far_future_fake(int64_t *time)
{
	/* Return a large negative value so that leo_satellite_search_timer_start()
	 * computes: delay = next_pass_unix_time_ms(0) - (-1e12) = 1e12 ms (~11 days).
	 * This prevents the LEO timer from firing during the test, allowing us to
	 * stay in WAITING_FOR_LEO long enough to test PERIODIC_TN_SEARCH.
	 */
	*time = -1000000000000LL;

	return 0;
}

void test_periodic_tn_search_transitions_to_searching_tn(void)
{
	struct network_msg msg;

	publish_network_msg(NETWORK_DISCONNECT);

	lte_lc_connect_async_fake.custom_fake = NULL;
	lte_lc_connect_async_fake.return_val = 0;

	publish_network_msg(NETWORK_CONNECT_NTN);

	/* Use a date_time_now fake that makes the LEO timer delay very large,
	 * preventing LEO_SATELLITE_PASS_UPCOMING from firing immediately.
	 */
	date_time_now_fake.custom_fake = date_time_now_far_future_fake;

	publish_priv_chan_msg(NTN_WAIT_FOR_SATELLITE_PASS);

	/* Restore fakes before sending PERIODIC_TN_SEARCH */
	date_time_now_fake.custom_fake = date_time_now_custom_fake;
	lte_lc_connect_async_fake.custom_fake = lte_lc_connect_async_custom_fake;

	publish_priv_chan_msg(PERIODIC_TN_SEARCH);

	wait_for_msg_of_type(&msg, NETWORK_CONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_CONNECTED, msg.type);
}

void test_psm_params_forwarded(void)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_PSM_UPDATE,
		.psm_cfg = {
			.tau = FAKE_PSM_TAU,
			.active_time = FAKE_PSM_ACTIVE_TIME,
		},
	};
	struct network_msg msg;

	TEST_ASSERT_NOT_NULL(lte_evt_handler);
	lte_evt_handler(&evt);

	wait_for_msg_of_type(&msg, NETWORK_PSM_PARAMS);

	TEST_ASSERT_EQUAL(NETWORK_PSM_PARAMS, msg.type);
	TEST_ASSERT_EQUAL(FAKE_PSM_TAU, msg.psm_cfg.tau);
	TEST_ASSERT_EQUAL(FAKE_PSM_ACTIVE_TIME, msg.psm_cfg.active_time);
}

void test_edrx_params_forwarded(void)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_EDRX_UPDATE,
		.edrx_cfg = {
			.mode = LTE_LC_LTE_MODE_LTEM,
			.edrx = FAKE_EDRX_VALUE,
			.ptw = FAKE_EDRX_PTW,
		},
	};
	struct network_msg msg;

	TEST_ASSERT_NOT_NULL(lte_evt_handler);
	lte_evt_handler(&evt);

	wait_for_msg_of_type(&msg, NETWORK_EDRX_PARAMS);

	TEST_ASSERT_EQUAL(NETWORK_EDRX_PARAMS, msg.type);
	TEST_ASSERT_EQUAL(LTE_LC_LTE_MODE_LTEM, msg.edrx_cfg.mode);
	TEST_ASSERT_EQUAL(FAKE_EDRX_VALUE, msg.edrx_cfg.edrx);
	TEST_ASSERT_EQUAL(FAKE_EDRX_PTW, msg.edrx_cfg.ptw);
}

void test_uicc_failure(void)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_NW_REG_STATUS,
		.nw_reg_status = LTE_LC_NW_REG_UICC_FAIL,
	};
	struct network_msg msg;

	TEST_ASSERT_NOT_NULL(lte_evt_handler);
	lte_evt_handler(&evt);

	wait_for_msg_of_type(&msg, NETWORK_UICC_FAILURE);
	TEST_ASSERT_EQUAL(NETWORK_UICC_FAILURE, msg.type);
}

void test_system_mode_request(void)
{
	struct network_msg msg;

	publish_network_msg(NETWORK_SYSTEM_MODE_REQUEST);

	wait_for_msg_of_type(&msg, NETWORK_SYSTEM_MODE_RESPONSE);
	TEST_ASSERT_EQUAL(NETWORK_SYSTEM_MODE_RESPONSE, msg.type);
	TEST_ASSERT_EQUAL(current_fake_system_mode, msg.system_mode);
}

void test_tle_data_stored_in_connected(void)
{
	struct network_msg msg;

	send_connect_tn();
	wait_for_msg_of_type(&msg, NETWORK_CONNECTED);

	struct network_msg tle_msg = {
		.type = NETWORK_TLE_DATA,
		.tle = {
			.line1 = "1 00000U 00000A   00000.00000000",
			.line2 = "2 00000  00.0000 000.0000 0000000",
		},
	};
	int err;

	err = zbus_chan_pub(&NETWORK_CHAN, &tle_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_MSEC(200));

	/* date_time_now should have been called for TLE timestamping */
	TEST_ASSERT_GREATER_OR_EQUAL(1, date_time_now_fake.call_count);
}

void test_location_failed_in_prepare(void)
{
	publish_network_msg(NETWORK_DISCONNECT);
	publish_network_msg(NETWORK_CONNECT_NTN);

	RESET_FAKE(lte_lc_func_mode_set);
	lte_lc_func_mode_set_fake.return_val = 0;

	publish_network_msg(NETWORK_LOCATION_FAILED);

	/* start_geo_or_maybe_reset() posts NTN_SEARCH_GEO_START internally,
	 * which transitions to GEO state. GEO entry calls start_geo_search()
	 * → network_connect() → lte_lc_func_mode_set(ACTIVATE_LTE).
	 */
	k_sleep(K_MSEC(200));

	TEST_ASSERT_GREATER_OR_EQUAL(1, lte_lc_func_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_FUNC_MODE_ACTIVATE_LTE,
			  lte_lc_func_mode_set_fake.arg0_val);
}

void test_no_suitable_cell_returns_to_idle(void)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_NW_REG_STATUS,
		.nw_reg_status = LTE_LC_NW_REG_NO_SUITABLE_CELL,
	};
	struct network_msg msg;

	/* Enter NTN search so the message is relevant */
	send_connect_ntn();

	TEST_ASSERT_NOT_NULL(lte_evt_handler);
	lte_evt_handler(&evt);

	wait_for_msg_of_type(&msg, NETWORK_NTN_NO_SUITABLE_CELL);
	TEST_ASSERT_EQUAL(NETWORK_NTN_NO_SUITABLE_CELL, msg.type);

	/* Should have returned to IDLE -- verify by sending CONNECT_TN */
	publish_network_msg(NETWORK_CONNECT_TN);

	wait_for_msg_of_type(&msg, NETWORK_CONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_CONNECTED, msg.type);
}

void test_geo_search_connects_via_location_failed(void)
{
	struct network_msg msg;

	send_connect_ntn();

	/* LOCATION_FAILED → start_geo_or_maybe_reset() → NTN_SEARCH_GEO_START → GEO entry
	 * GEO entry calls start_geo_search() → network_connect() → ACTIVATE_LTE.
	 * The mode-aware fake fires PDN_ACTIVATED → NETWORK_CONNECTED.
	 */
	publish_network_msg(NETWORK_LOCATION_FAILED);

	wait_for_msg_of_type(&msg, NETWORK_CONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_CONNECTED, msg.type);

	/* Verify we ended up in CONNECTED by sending DISCONNECT */
	publish_network_msg(NETWORK_DISCONNECT);

	wait_for_msg_of_type(&msg, NETWORK_DISCONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_DISCONNECTED, msg.type);
}

void test_attach_rejected_forwarded(void)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_NW_REG_STATUS,
		.nw_reg_status = LTE_LC_NW_REG_NOT_REGISTERED,
	};
	struct network_msg msg;

	TEST_ASSERT_NOT_NULL(lte_evt_handler);
	lte_evt_handler(&evt);

	wait_for_msg_of_type(&msg, NETWORK_ATTACH_REJECTED);
	TEST_ASSERT_EQUAL(NETWORK_ATTACH_REJECTED, msg.type);
}

void test_modem_reset_loop_forwarded(void)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_MODEM_EVENT,
		.modem_evt = {
			.type = LTE_LC_MODEM_EVT_RESET_LOOP,
		},
	};
	struct network_msg msg;

	TEST_ASSERT_NOT_NULL(lte_evt_handler);
	lte_evt_handler(&evt);

	wait_for_msg_of_type(&msg, NETWORK_MODEM_RESET_LOOP);
	TEST_ASSERT_EQUAL(NETWORK_MODEM_RESET_LOOP, msg.type);
}

void test_light_search_done_forwarded(void)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_MODEM_EVENT,
		.modem_evt = {
			.type = LTE_LC_MODEM_EVT_LIGHT_SEARCH_DONE,
		},
	};
	struct network_msg msg;

	TEST_ASSERT_NOT_NULL(lte_evt_handler);
	lte_evt_handler(&evt);

	wait_for_msg_of_type(&msg, NETWORK_LIGHT_SEARCH_DONE);
	TEST_ASSERT_EQUAL(NETWORK_LIGHT_SEARCH_DONE, msg.type);
}

void test_search_done_forwarded(void)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_MODEM_EVENT,
		.modem_evt = {
			.type = LTE_LC_MODEM_EVT_SEARCH_DONE,
		},
	};
	struct network_msg msg;

	TEST_ASSERT_NOT_NULL(lte_evt_handler);
	lte_evt_handler(&evt);

	wait_for_msg_of_type(&msg, NETWORK_SEARCH_DONE);
	TEST_ASSERT_EQUAL(NETWORK_SEARCH_DONE, msg.type);
}

void test_pdn_network_detach_forwards_disconnected(void)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_PDN,
		.pdn = {
			.type = LTE_LC_EVT_PDN_NETWORK_DETACH,
			.cid = 0,
		},
	};
	struct network_msg msg;

	TEST_ASSERT_NOT_NULL(lte_evt_handler);
	lte_evt_handler(&evt);

	wait_for_msg_of_type(&msg, NETWORK_DISCONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_DISCONNECTED, msg.type);
}

void test_pdn_suspended_forwards_disconnected(void)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_PDN,
		.pdn = {
			.type = LTE_LC_EVT_PDN_SUSPENDED,
			.cid = 0,
		},
	};
	struct network_msg msg;

	TEST_ASSERT_NOT_NULL(lte_evt_handler);
	lte_evt_handler(&evt);

	wait_for_msg_of_type(&msg, NETWORK_DISCONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_DISCONNECTED, msg.type);
}

void test_pdn_resumed_forwards_connected(void)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_PDN,
		.pdn = {
			.type = LTE_LC_EVT_PDN_RESUMED,
			.cid = 0,
		},
	};
	struct network_msg msg;

	TEST_ASSERT_NOT_NULL(lte_evt_handler);
	lte_evt_handler(&evt);

	wait_for_msg_of_type(&msg, NETWORK_CONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_CONNECTED, msg.type);
}

void test_system_mode_set_ltem_in_idle(void)
{
	publish_disconnect_msg();

	RESET_FAKE(lte_lc_system_mode_set);
	lte_lc_system_mode_set_fake.custom_fake = lte_lc_system_mode_set_custom_fake;

	publish_network_msg(NETWORK_SYSTEM_MODE_SET_LTEM);

	TEST_ASSERT_EQUAL(1, lte_lc_system_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_SYSTEM_MODE_LTEM_GPS,
			  lte_lc_system_mode_set_fake.arg0_val);
}

void test_system_mode_set_nbiot_in_idle(void)
{
	publish_disconnect_msg();

	RESET_FAKE(lte_lc_system_mode_set);
	lte_lc_system_mode_set_fake.custom_fake = lte_lc_system_mode_set_custom_fake;

	publish_network_msg(NETWORK_SYSTEM_MODE_SET_NBIOT);

	TEST_ASSERT_EQUAL(1, lte_lc_system_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_SYSTEM_MODE_NBIOT_GPS,
			  lte_lc_system_mode_set_fake.arg0_val);
}

void test_system_mode_set_ltem_nbiot_in_idle(void)
{
	publish_disconnect_msg();

	RESET_FAKE(lte_lc_system_mode_set);
	lte_lc_system_mode_set_fake.custom_fake = lte_lc_system_mode_set_custom_fake;

	publish_network_msg(NETWORK_SYSTEM_MODE_SET_LTEM_NBIOT);

	TEST_ASSERT_EQUAL(1, lte_lc_system_mode_set_fake.call_count);
	TEST_ASSERT_EQUAL(LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS,
			  lte_lc_system_mode_set_fake.arg0_val);
}

void test_search_stop_returns_to_idle(void)
{
	struct network_msg msg;

	publish_disconnect_msg();

	/* Enter SEARCHING_TN without auto-connecting */
	lte_lc_connect_async_fake.custom_fake = NULL;
	lte_lc_connect_async_fake.return_val = 0;

	publish_network_msg(NETWORK_CONNECT_TN);

	/* SEARCH_STOP should return to IDLE */
	publish_network_msg(NETWORK_SEARCH_STOP);

	/* Verify we are in IDLE by successfully connecting */
	lte_lc_connect_async_fake.custom_fake = lte_lc_connect_async_custom_fake;

	publish_network_msg(NETWORK_CONNECT_TN);

	wait_for_msg_of_type(&msg, NETWORK_CONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_CONNECTED, msg.type);
}

void test_leo_search_to_connected(void)
{
	struct network_msg msg;

	send_connect_ntn();

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

void test_disconnect_while_idle_is_noop(void)
{
	publish_disconnect_msg();

	RESET_FAKE(lte_lc_func_mode_set);
	lte_lc_func_mode_set_fake.return_val = 0;

	publish_network_msg(NETWORK_DISCONNECT);

	/* IDLE swallows DISCONNECT without calling network_disconnect() */
	TEST_ASSERT_EQUAL(0, lte_lc_func_mode_set_fake.call_count);

	/* Confirm still in IDLE by accepting CONNECT_TN */
	publish_network_msg(NETWORK_CONNECT_TN);

	struct network_msg msg;

	wait_for_msg_of_type(&msg, NETWORK_CONNECTED);
	TEST_ASSERT_EQUAL(NETWORK_CONNECTED, msg.type);
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
