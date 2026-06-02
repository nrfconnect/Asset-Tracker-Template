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
#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
#include <modem/ntn.h>

#include "app_common.h"
#include "network.h"

LOG_MODULE_REGISTER(network_module_test, LOG_LEVEL_DBG);

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(int, nrf_modem_lib_init);
FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);
FAKE_VOID_FUNC(lte_lc_register_handler, lte_lc_evt_handler_t);
FAKE_VALUE_FUNC(int, lte_lc_offline);
FAKE_VALUE_FUNC(int, lte_lc_connect_async, lte_lc_evt_handler_t);
FAKE_VALUE_FUNC(int, lte_lc_pdn_default_ctx_events_enable);
FAKE_VALUE_FUNC(int, lte_lc_normal);
FAKE_VALUE_FUNC(int, lte_lc_func_mode_set, enum lte_lc_func_mode);
FAKE_VALUE_FUNC(int, lte_lc_system_mode_set, enum lte_lc_system_mode,
		enum lte_lc_system_mode_preference);
FAKE_VALUE_FUNC(int, lte_lc_system_mode_get, enum lte_lc_system_mode *,
		enum lte_lc_system_mode_preference *);
FAKE_VALUE_FUNC(int, lte_lc_cellular_profile_configure,
		struct lte_lc_cellular_profile *);
FAKE_VOID_FUNC(ntn_register_handler, ntn_evt_handler_t);
FAKE_VALUE_FUNC(int, ntn_location_set, double, double, float, uint32_t);

ZBUS_MSG_SUBSCRIBER_DEFINE(test_subscriber);
ZBUS_CHAN_ADD_OBS(network_chan, test_subscriber, 0);

static lte_lc_evt_handler_t module_lte_handler;
static ntn_evt_handler_t module_ntn_handler;
static enum lte_lc_system_mode test_modem_sys_mode = LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS;

static void ntn_register_handler_custom_fake(ntn_evt_handler_t handler)
{
	module_ntn_handler = handler;
}

static int lte_lc_system_mode_set_custom_fake(enum lte_lc_system_mode mode,
					      enum lte_lc_system_mode_preference preference)
{
	ARG_UNUSED(preference);

	test_modem_sys_mode = mode;

	return 0;
}

static int lte_lc_system_mode_get_custom_fake(enum lte_lc_system_mode *mode,
					      enum lte_lc_system_mode_preference *preference)
{
	if (mode != NULL) {
		*mode = test_modem_sys_mode;
	}

	if (preference != NULL) {
		*preference = LTE_LC_SYSTEM_MODE_PREFER_AUTO;
	}

	return 0;
}

static void lte_lc_register_handler_custom_fake(lte_lc_evt_handler_t handler)
{
	module_lte_handler = handler;
}

static int lte_lc_pdn_default_ctx_events_enable_custom_fake(void)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_PDN,
		.pdn = { .type = LTE_LC_EVT_PDN_DEACTIVATED, .cid = 0 },
	};

	if (module_lte_handler != NULL) {
		module_lte_handler(&evt);
	}

	return 0;
}

static int lte_lc_connect_async_custom_fake(lte_lc_evt_handler_t handler)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_PDN,
		.pdn = { .type = LTE_LC_EVT_PDN_ACTIVATED, .cid = 0 },
	};

	module_lte_handler = handler;

	if (module_lte_handler != NULL) {
		module_lte_handler(&evt);
	}

	return 0;
}

static int lte_lc_offline_custom_fake(void)
{
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_PDN,
		.pdn = { .type = LTE_LC_EVT_PDN_DEACTIVATED, .cid = 0 },
	};

	if (module_lte_handler != NULL) {
		module_lte_handler(&evt);
	}

	return 0;
}

static int lte_lc_connect_async_search_only_fake(lte_lc_evt_handler_t handler)
{
	module_lte_handler = handler;

	return 0;
}

static void purge_network_messages(void)
{
	const struct zbus_channel *chan;
	struct network_msg msg;

	while (zbus_sub_wait_msg(&test_subscriber, &chan, &msg, K_NO_WAIT) == 0) {
		;
	}
}

static void wait_for_msg(struct network_msg *msg, enum network_msg_type expected)
{
	const struct zbus_channel *chan;
	int err;
	uint64_t end_time = k_uptime_get() + 3000;

	while (k_uptime_get() < end_time) {
		err = zbus_sub_wait_msg(&test_subscriber, &chan, msg, K_MSEC(200));
		if (err == -ENOMSG) {
			continue;
		}

		TEST_ASSERT_EQUAL(0, err);
		TEST_ASSERT_EQUAL_PTR(&network_chan, chan);

		if (msg->type == expected) {
			return;
		}

		LOG_DBG("Ignoring network message type %d", msg->type);
	}

	TEST_FAIL_MESSAGE("Timeout waiting for network message");
}

void setUp(void)
{
	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(task_wdt_add);
	RESET_FAKE(lte_lc_offline);
	RESET_FAKE(lte_lc_connect_async);
	RESET_FAKE(lte_lc_pdn_default_ctx_events_enable);
	RESET_FAKE(lte_lc_normal);
	RESET_FAKE(lte_lc_func_mode_set);
	RESET_FAKE(lte_lc_system_mode_set);
	RESET_FAKE(lte_lc_system_mode_get);
	RESET_FAKE(lte_lc_cellular_profile_configure);
	RESET_FAKE(lte_lc_register_handler);
	RESET_FAKE(nrf_modem_lib_init);
	RESET_FAKE(ntn_register_handler);
	RESET_FAKE(ntn_location_set);

	test_modem_sys_mode = LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS;
	lte_lc_system_mode_set_fake.custom_fake = lte_lc_system_mode_set_custom_fake;
	lte_lc_system_mode_get_fake.custom_fake = lte_lc_system_mode_get_custom_fake;
	lte_lc_register_handler_fake.custom_fake = lte_lc_register_handler_custom_fake;
	lte_lc_connect_async_fake.custom_fake = lte_lc_connect_async_custom_fake;
	lte_lc_offline_fake.custom_fake = lte_lc_offline_custom_fake;
	lte_lc_pdn_default_ctx_events_enable_fake.custom_fake =
		lte_lc_pdn_default_ctx_events_enable_custom_fake;
	lte_lc_cellular_profile_configure_fake.return_val = 0;
	ntn_location_set_fake.return_val = 0;
	ntn_register_handler_fake.custom_fake = ntn_register_handler_custom_fake;

	k_sleep(K_MSEC(100));

	if (lte_lc_register_handler_fake.call_count > 0) {
		module_lte_handler = lte_lc_register_handler_fake.arg0_history[0];
	}
}

void tearDown(void)
{
	purge_network_messages();
}

void test_network_ntn_boot_disconnected(void)
{
	struct network_msg msg;

	wait_for_msg(&msg, NETWORK_DISCONNECTED);
}

void test_network_ntn_connect_tn(void)
{
	struct network_msg msg = { .type = NETWORK_CONNECT_TN };
	int err;

	err = zbus_chan_pub(&network_chan, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_MSEC(50));
	wait_for_msg(&msg, NETWORK_CONNECTED_TN);
}

void test_network_ntn_tn_search_failed(void)
{
	struct network_msg msg = { .type = NETWORK_DISCONNECT };
	struct lte_lc_evt pdn_down = {
		.type = LTE_LC_EVT_PDN,
		.pdn = { .type = LTE_LC_EVT_PDN_DEACTIVATED, .cid = 0 },
	};
	struct lte_lc_evt search_done = {
		.type = LTE_LC_EVT_MODEM_EVENT,
		.modem_evt.type = LTE_LC_MODEM_EVT_SEARCH_DONE,
	};
	int err;

	/* Return to idle after prior connect test */
	err = zbus_chan_pub(&network_chan, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_MSEC(50));
	TEST_ASSERT_NOT_NULL(module_lte_handler);
	module_lte_handler(&pdn_down);
	wait_for_msg(&msg, NETWORK_DISCONNECTED);

	lte_lc_connect_async_fake.custom_fake = lte_lc_connect_async_search_only_fake;

	msg.type = NETWORK_CONNECT_TN;
	err = zbus_chan_pub(&network_chan, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_MSEC(100));
	TEST_ASSERT_NOT_NULL(module_lte_handler);

	module_lte_handler(&search_done);

	wait_for_msg(&msg, NETWORK_TN_SEARCH_FAILED);

	lte_lc_connect_async_fake.custom_fake = lte_lc_connect_async_custom_fake;
}

void test_network_ntn_psm_params_update(void)
{
	struct network_msg msg;
	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_PSM_UPDATE,
		.psm_cfg = { .tau = 3600, .active_time = 16 },
	};

	if (module_lte_handler != NULL) {
		module_lte_handler(&evt);
	}

	wait_for_msg(&msg, NETWORK_PSM_PARAMS);
	TEST_ASSERT_EQUAL(3600, msg.psm_cfg.tau);
	TEST_ASSERT_EQUAL(16, msg.psm_cfg.active_time);
}

/* Drive the module back to STATE_DISCONNECTED_IDLE from any state and drain the channel.
 * A DISCONNECT from a connected state enters STATE_DISCONNECTING, which waits for the modem's
 * PDN-deactivated notification, so feed that event in to complete the transition.
 */
static void return_to_idle(void)
{
	struct network_msg msg = { .type = NETWORK_DISCONNECT };
	struct lte_lc_evt pdn_down = {
		.type = LTE_LC_EVT_PDN,
		.pdn = { .type = LTE_LC_EVT_PDN_DEACTIVATED, .cid = 0 },
	};

	(void)zbus_chan_pub(&network_chan, &msg, K_SECONDS(1));
	k_sleep(K_MSEC(50));

	if (module_lte_handler != NULL) {
		module_lte_handler(&pdn_down);
	}

	k_sleep(K_MSEC(50));
	purge_network_messages();
}

/* NTN tests below share the module's static location cache and therefore depend on order:
 * the two tests that exercise the GNSS-acquisition path must run before any test caches a
 * location, otherwise the fresh cache makes the module skip straight to the cell search.
 */

/* No cached location: NETWORK_CONNECT_NTN must request a GNSS fix, and a failed fix must
 * abort the attempt with NETWORK_NTN_SEARCH_FAILED. Must run before a location is cached.
 */
void test_network_ntn_gnss_request_then_failed(void)
{
	struct network_msg msg = { .type = NETWORK_CONNECT_NTN };
	int err;

	return_to_idle();

	err = zbus_chan_pub(&network_chan, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	wait_for_msg(&msg, NETWORK_GNSS_LOCATION_REQ);

	/* Modem should have been switched to GPS-only system mode to acquire the fix. */
	TEST_ASSERT_EQUAL(LTE_LC_SYSTEM_MODE_GPS, test_modem_sys_mode);

	msg.type = NETWORK_GNSS_LOCATION_FAILED;
	err = zbus_chan_pub(&network_chan, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	wait_for_msg(&msg, NETWORK_NTN_SEARCH_FAILED);

	/* A failed fix must not populate the cache. */
	TEST_ASSERT_EQUAL(0, ntn_location_set_fake.call_count);
}

/* No cached location: after requesting a fix, providing NETWORK_GNSS_LOCATION feeds it to the
 * modem and the NTN cell search connects. Must run before the reuse test (it caches a fix).
 */
void test_network_ntn_gnss_request_then_connects(void)
{
	struct network_msg msg = { .type = NETWORK_CONNECT_NTN };
	int err;

	return_to_idle();
	RESET_FAKE(ntn_location_set);
	ntn_location_set_fake.return_val = 0;

	err = zbus_chan_pub(&network_chan, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	wait_for_msg(&msg, NETWORK_GNSS_LOCATION_REQ);

	msg.type = NETWORK_GNSS_LOCATION;
	msg.location.lat = 63.42;
	msg.location.lon = 10.43;
	msg.location.alt = 12.0f;
	err = zbus_chan_pub(&network_chan, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	wait_for_msg(&msg, NETWORK_CONNECTED_NTN);

	/* The fix must have been forwarded to the modem and the modem put in NTN mode.
	 * Unity floating-point asserts are disabled in this build, so compare with a tolerance.
	 */
	TEST_ASSERT_GREATER_OR_EQUAL(1, ntn_location_set_fake.call_count);
	TEST_ASSERT_TRUE((ntn_location_set_fake.arg0_val > 63.41) &&
			 (ntn_location_set_fake.arg0_val < 63.43));
	TEST_ASSERT_TRUE((ntn_location_set_fake.arg1_val > 10.42) &&
			 (ntn_location_set_fake.arg1_val < 10.44));
	TEST_ASSERT_EQUAL(LTE_LC_SYSTEM_MODE_NTN_NBIOT, test_modem_sys_mode);
}

/* Cached fix present (from the previous test): NETWORK_CONNECT_NTN must connect without
 * requesting a new GNSS fix. Reaching NETWORK_CONNECTED_NTN without us providing a location
 * proves the cached path (CHECK_LOCATION -> CELL_SEARCH) was taken.
 */
void test_network_ntn_reconnect_uses_cached_location(void)
{
	struct network_msg msg = { .type = NETWORK_CONNECT_NTN };
	int err;

	return_to_idle();
	RESET_FAKE(ntn_location_set);
	ntn_location_set_fake.return_val = 0;

	err = zbus_chan_pub(&network_chan, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	wait_for_msg(&msg, NETWORK_CONNECTED_NTN);

	/* Cached fix must have been re-fed to the modem. */
	TEST_ASSERT_GREATER_OR_EQUAL(1, ntn_location_set_fake.call_count);
}

/* Cached fix present: if the NTN cell search finds no cell, the modem reports SEARCH_DONE
 * while in NTN mode, which must surface as NETWORK_NTN_SEARCH_FAILED.
 */
void test_network_ntn_cell_search_failed(void)
{
	struct network_msg msg = { .type = NETWORK_CONNECT_NTN };
	struct lte_lc_evt search_done = {
		.type = LTE_LC_EVT_MODEM_EVENT,
		.modem_evt.type = LTE_LC_MODEM_EVT_SEARCH_DONE,
	};
	int err;

	return_to_idle();

	/* Do not auto-connect, so the module stays in the cell search. */
	lte_lc_connect_async_fake.custom_fake = lte_lc_connect_async_search_only_fake;

	err = zbus_chan_pub(&network_chan, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_MSEC(100));
	TEST_ASSERT_NOT_NULL(module_lte_handler);
	TEST_ASSERT_EQUAL(LTE_LC_SYSTEM_MODE_NTN_NBIOT, test_modem_sys_mode);

	module_lte_handler(&search_done);

	wait_for_msg(&msg, NETWORK_NTN_SEARCH_FAILED);

	lte_lc_connect_async_fake.custom_fake = lte_lc_connect_async_custom_fake;
}

/* Cached fix present: a modem NTN_EVT_LOCATION_REQUEST must re-feed the cached fix to the
 * modem without requiring a new GNSS acquisition.
 */
void test_network_ntn_modem_location_request_refeeds_cache(void)
{
	struct ntn_evt evt = {
		.type = NTN_EVT_LOCATION_REQUEST,
		.location_request = { .requested = true, .accuracy = 1000 },
	};
	int count_before;

	return_to_idle();
	TEST_ASSERT_NOT_NULL(module_ntn_handler);

	count_before = ntn_location_set_fake.call_count;

	module_ntn_handler(&evt);
	k_sleep(K_MSEC(100));

	TEST_ASSERT_GREATER_THAN(count_before, ntn_location_set_fake.call_count);
}

/* Cached fix present (from the earlier NTN tests): NETWORK_CONNECT_NTN with .fresh_location set
 * must request a new GNSS fix even though the cached fix is still valid, so the fix can double
 * as a location sample.
 */
void test_network_ntn_fresh_location_forces_acquisition(void)
{
	struct network_msg msg = {
		.type = NETWORK_CONNECT_NTN,
		.fresh_location = true,
	};
	int err;

	return_to_idle();
	RESET_FAKE(ntn_location_set);
	ntn_location_set_fake.return_val = 0;

	err = zbus_chan_pub(&network_chan, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Despite the fresh cache, a new fix must be requested. */
	wait_for_msg(&msg, NETWORK_GNSS_LOCATION_REQ);

	msg.type = NETWORK_GNSS_LOCATION;
	msg.location.lat = 63.43;
	msg.location.lon = 10.44;
	msg.location.alt = 15.0f;
	err = zbus_chan_pub(&network_chan, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	wait_for_msg(&msg, NETWORK_CONNECTED_NTN);
	TEST_ASSERT_GREATER_OR_EQUAL(1, ntn_location_set_fake.call_count);

	/* The fresh request is one-shot: a subsequent plain CONNECT_NTN must use the cache
	 * (reaching CONNECTED_NTN without a new GNSS request proves the cached path).
	 */
	return_to_idle();

	msg = (struct network_msg){ .type = NETWORK_CONNECT_NTN };
	err = zbus_chan_pub(&network_chan, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	wait_for_msg(&msg, NETWORK_CONNECTED_NTN);
}

/* Registration-status driven connectivity. These run after the NTN tests and use TN, so they
 * do not interfere with the location-cache ordering above.
 */

/* Losing network registration while connected (e.g. +CEREG: 4, out of coverage) must surface as
 * NETWORK_DISCONNECTED even though the PDN context has not been deactivated, so the cloud session
 * is paused promptly rather than left to fail over a dead radio.
 */
void test_network_registration_loss_disconnects(void)
{
	struct network_msg msg = { .type = NETWORK_CONNECT_TN };
	struct lte_lc_evt reg_home = {
		.type = LTE_LC_EVT_NW_REG_STATUS,
		.nw_reg_status = LTE_LC_NW_REG_REGISTERED_HOME,
	};
	struct lte_lc_evt reg_lost = {
		.type = LTE_LC_EVT_NW_REG_STATUS,
		.nw_reg_status = LTE_LC_NW_REG_UNKNOWN,
	};
	int err;

	return_to_idle();

	/* Connect over TN. The connect fake fires PDN_ACTIVATED, so the module's PDN is active. */
	err = zbus_chan_pub(&network_chan, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
	wait_for_msg(&msg, NETWORK_CONNECTED_TN);

	TEST_ASSERT_NOT_NULL(module_lte_handler);

	/* Become registered, then lose registration while the PDN stays up. */
	module_lte_handler(&reg_home);
	k_sleep(K_MSEC(50));
	purge_network_messages();

	module_lte_handler(&reg_lost);

	wait_for_msg(&msg, NETWORK_DISCONNECTED);
}

/* Regaining registration while the PDN context is still active (a coverage blip that did not tear
 * down the bearer) must re-assert connectivity so the paused session can resume - the modem emits
 * no new PDN activation in this case.
 */
void test_network_registration_regain_resumes(void)
{
	struct network_msg msg = { .type = NETWORK_CONNECT_TN };
	struct lte_lc_evt reg_home = {
		.type = LTE_LC_EVT_NW_REG_STATUS,
		.nw_reg_status = LTE_LC_NW_REG_REGISTERED_HOME,
	};
	struct lte_lc_evt reg_lost = {
		.type = LTE_LC_EVT_NW_REG_STATUS,
		.nw_reg_status = LTE_LC_NW_REG_UNKNOWN,
	};
	int err;

	return_to_idle();

	err = zbus_chan_pub(&network_chan, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
	wait_for_msg(&msg, NETWORK_CONNECTED_TN);

	TEST_ASSERT_NOT_NULL(module_lte_handler);

	module_lte_handler(&reg_home);
	k_sleep(K_MSEC(50));
	module_lte_handler(&reg_lost);
	wait_for_msg(&msg, NETWORK_DISCONNECTED);
	purge_network_messages();

	/* PDN was never deactivated, so registration coming back must resume connectivity. */
	module_lte_handler(&reg_home);
	wait_for_msg(&msg, NETWORK_CONNECTED_TN);
}

/* While connected, a fresh NETWORK_CONNECT_TN (Main re-driving the connection to recover a paused
 * cloud session) must re-assert the current connectivity instead of being ignored.
 */
void test_network_connect_tn_while_connected_reasserts(void)
{
	struct network_msg msg = { .type = NETWORK_CONNECT_TN };
	int err;

	return_to_idle();

	err = zbus_chan_pub(&network_chan, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
	wait_for_msg(&msg, NETWORK_CONNECTED_TN);
	purge_network_messages();

	msg.type = NETWORK_CONNECT_TN;
	err = zbus_chan_pub(&network_chan, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	wait_for_msg(&msg, NETWORK_CONNECTED_TN);
}

extern int unity_main(void);

int main(void)
{
	(void)unity_main();

	return 0;
}
