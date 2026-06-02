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
#include <date_time.h>

#include "dk_buttons_and_leds.h"
#include "app_common.h"
#include "power.h"
#include "network.h"
#include "environmental.h"
#include "cloud.h"
#include "fota.h"
#include "location.h"
#include "led.h"
#include "button.h"
#include "storage.h"
#include "checks.h"
#include "cbor_helper.h"

DEFINE_FFF_GLOBALS;

#define HOUR_IN_SECONDS 3600
#define WEEK_IN_SECONDS HOUR_IN_SECONDS * 24 * 7

FAKE_VALUE_FUNC(int, dk_buttons_init, button_handler_t);
FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);
FAKE_VOID_FUNC(sys_reboot, int);

LOG_MODULE_REGISTER(main_module_test, 4);

/* Define the channels for testing */
ZBUS_CHAN_DEFINE(power_chan,
	struct power_msg,
	NULL,
	NULL,
	ZBUS_OBSERVERS_EMPTY,
	ZBUS_MSG_INIT(0)
);
ZBUS_CHAN_DEFINE(button_chan,
	struct button_msg,
	NULL,
	NULL,
	ZBUS_OBSERVERS_EMPTY,
	ZBUS_MSG_INIT(0)
);
ZBUS_CHAN_DEFINE(network_chan,
	struct network_msg,
	NULL,
	NULL,
	ZBUS_OBSERVERS_EMPTY,
	ZBUS_MSG_INIT(.type = NETWORK_DISCONNECTED)
);
ZBUS_CHAN_DEFINE(cloud_chan,
	struct cloud_msg,
	NULL,
	NULL,
	ZBUS_OBSERVERS_EMPTY,
	ZBUS_MSG_INIT(.type = CLOUD_DISCONNECTED)
);
ZBUS_CHAN_DEFINE(environmental_chan,
	struct environmental_msg,
	NULL,
	NULL,
	ZBUS_OBSERVERS_EMPTY,
	ZBUS_MSG_INIT(0)
);
ZBUS_CHAN_DEFINE(fota_chan,
	struct fota_msg,
	NULL,
	NULL,
	ZBUS_OBSERVERS_EMPTY,
	ZBUS_MSG_INIT(0)
);
ZBUS_CHAN_DEFINE(location_chan,
	struct location_msg,
	NULL,
	NULL,
	ZBUS_OBSERVERS_EMPTY,
	ZBUS_MSG_INIT(0)
);
ZBUS_CHAN_DEFINE(led_chan,
	struct led_msg,
	NULL,
	NULL,
	ZBUS_OBSERVERS_EMPTY,
	ZBUS_MSG_INIT(0)
);
ZBUS_CHAN_DEFINE(storage_chan,
	struct storage_msg,
	NULL,
	NULL,
	ZBUS_OBSERVERS_EMPTY,
	ZBUS_MSG_INIT(0)
);

/* Helper functions for sending messages */

static void send_cloud_connected(void)
{
	struct cloud_msg cloud_msg = {
		.type = CLOUD_CONNECTED,
	};

	int err = zbus_chan_pub(&cloud_chan, &cloud_msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_cloud_disconnected(void)
{
	struct cloud_msg cloud_msg = {
		.type = CLOUD_DISCONNECTED,
	};

	int err = zbus_chan_pub(&cloud_chan, &cloud_msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_cloud_session_established(void)
{
	struct cloud_msg cloud_msg = {
		.type = CLOUD_SESSION_ESTABLISHED,
	};

	int err = zbus_chan_pub(&cloud_chan, &cloud_msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_cloud_session_stopped(void)
{
	struct cloud_msg cloud_msg = {
		.type = CLOUD_SESSION_STOPPED,
	};

	int err = zbus_chan_pub(&cloud_chan, &cloud_msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_network_disconnected(void)
{
	struct network_msg network_msg = {
		.type = NETWORK_DISCONNECTED,
	};

	int err = zbus_chan_pub(&network_chan, &network_msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_network_tn_search_failed(void)
{
	struct network_msg network_msg = {
		.type = NETWORK_TN_SEARCH_FAILED,
	};

	int err = zbus_chan_pub(&network_chan, &network_msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_network_ntn_search_failed(void)
{
	struct network_msg network_msg = {
		.type = NETWORK_NTN_SEARCH_FAILED,
	};

	int err = zbus_chan_pub(&network_chan, &network_msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_network_connected_tn(void)
{
	struct network_msg network_msg = {
		.type = NETWORK_CONNECTED_TN,
	};

	int err = zbus_chan_pub(&network_chan, &network_msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_network_connected_ntn(void)
{
	struct network_msg network_msg = {
		.type = NETWORK_CONNECTED_NTN,
	};

	int err = zbus_chan_pub(&network_chan, &network_msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_location_search_done(void)
{
	struct location_msg msg = {
		.type = LOCATION_SEARCH_DONE,
	};

	int err = zbus_chan_pub(&location_chan, &msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_network_gnss_location_req(void)
{
	struct network_msg network_msg = {
		.type = NETWORK_GNSS_LOCATION_REQ,
	};

	int err = zbus_chan_pub(&network_chan, &network_msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_location_gnss_data(double lat, double lon, float alt)
{
	struct location_msg msg = {
		.type = LOCATION_GNSS_DATA,
	};
	int err;

	msg.gnss_data.latitude = lat;
	msg.gnss_data.longitude = lon;
	msg.gnss_data.details.gnss.pvt_data.altitude = alt;

	err = zbus_chan_pub(&location_chan, &msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_location_ready(void)
{
	struct location_msg msg = {
		.type = LOCATION_MODULE_READY,
	};

	int err = zbus_chan_pub(&location_chan, &msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_power_ready(void)
{
	struct power_msg msg = {
		.type = POWER_MODULE_READY,
	};

	int err = zbus_chan_pub(&power_chan, &msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_button_press_short(void)
{
	struct button_msg button_msg = {
		.type = BUTTON_PRESS_SHORT,
		.button_number = 1
	};

	int err = zbus_chan_pub(&button_chan, &button_msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_button_press_long(void)
{
	struct button_msg button_msg = {
		.type = BUTTON_PRESS_LONG,
		.button_number = 1
	};

	int err = zbus_chan_pub(&button_chan, &button_msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_storage_threshold_reached(void)
{
	struct storage_msg storage_msg = {
		.type = STORAGE_THRESHOLD_REACHED,
	};
	int err = zbus_chan_pub(&storage_chan, &storage_msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_storage_batch_close(void)
{
	struct storage_msg storage_msg = {
		.type = STORAGE_BATCH_CLOSE,
	};
	int err = zbus_chan_pub(&storage_chan, &storage_msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void send_fota_msg(enum fota_msg_type msg_type)
{
	int err;
	struct fota_msg msg = { .type = msg_type };

	LOG_INF("Sending FOTA message: %d", msg_type);

	err = zbus_chan_pub(&fota_chan, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_MSEC(100));
}

static void config_change_sampling_interval(uint32_t sampling_interval)
{
	int err;
	struct cloud_msg msg = {
		.type = CLOUD_SHADOW_RESPONSE_DELTA,
	};
	struct config_params config = {
		.sample_interval = sampling_interval,
	};
	size_t encoded_len = 0;

	err = encode_shadow_parameters_to_cbor(&config, 0, 0, msg.response.buffer,
							 sizeof(msg.response.buffer), &encoded_len);
	if (err != 0) {
		TEST_FAIL_MESSAGE("Failed to encode CBOR parameters");
	}

	msg.response.buffer_data_len = encoded_len;

	err = zbus_chan_pub(&cloud_chan, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
}

static void config_change_all(uint32_t sample_interval, uint32_t storage_threshold)
{
	int err;
	struct cloud_msg msg = {
		.type = CLOUD_SHADOW_RESPONSE_DELTA,
	};
	struct config_params config = {
		.sample_interval = sample_interval,
		.storage_threshold = storage_threshold,
		.storage_threshold_valid = true,
	};
	size_t encoded_len = 0;

	err = encode_shadow_parameters_to_cbor(&config, 0, 0, msg.response.buffer,
					       sizeof(msg.response.buffer), &encoded_len);
	if (err != 0) {
		TEST_FAIL_MESSAGE("Failed to encode CBOR parameters");
	}

	msg.response.buffer_data_len = encoded_len;

	err = zbus_chan_pub(&cloud_chan, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
}

/* Restart the sampling timer by doing a immediate sample using a short button press */
static void restart_sample_timer(void)
{
	send_button_press_short();
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
}

/* Connect to cloud and complete the initial data dispatch cycle.
 * STATE_CONNECTED_SENDING is the initial child state of STATE_CONNECTED,
 * so connecting always triggers an immediate data dispatch via cloud_send_now().
 */
static void connect_to_cloud(void)
{
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);
}

void setUp(void)
{
	RESET_FAKE(dk_buttons_init);
	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(task_wdt_add);
	RESET_FAKE(sys_reboot);

	/* Send ready messages for all relevant modules */
	send_location_ready();
	send_power_ready();
	send_fota_msg(FOTA_MODULE_READY);

	/* Ensure clean disconnected state. Also clear any cloud session left active by a previous
	 * test, since the gate controlling NTN fallback (cloud_session_active in main) persists
	 * across tests with the singleton state machine and is only cleared by CLOUD_SESSION_STOPPED.
	 */
	send_cloud_disconnected();
	send_cloud_session_stopped();

	/* Losing the cloud connection now schedules a TN reconnect back-off. Cancel any such
	 * pending reconnect left by a previous test (or by the disconnect just above) so it does
	 * not fire mid-test; a NETWORK_CONNECTED_TN resets the back-off without changing state.
	 */
	send_network_connected_tn();
	k_sleep(K_MSEC(500));

	/* Trigger a button press to "burn" the current sampling cycle and reset state.
	 * This bypasses the "too soon to sample" protection and ensures each test
	 * starts with a known, clean state.
	 */
	send_button_press_short();
	send_location_search_done();
	/* Wait for sampling to complete and return to waiting state */
	k_sleep(K_MSEC(100));

	FFF_RESET_HISTORY();

	/* Clear any stale events from cleanup */
	purge_all_events();
}

/* Test functions */

void test_init_first_connection(void)
{
	/* Connect to cloud */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);

	/* First connection should trigger fota poll and get shadow desired */
	expect_cloud_event(CLOUD_SHADOW_UPDATE_REPORTED_DEVICE);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DESIRED);
}

void test_short_button_press_connected(void)
{
	connect_to_cloud();

	/* Short button press should trigger sampling immediately */
	send_button_press_short();
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Complete location search */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
}

void test_long_button_press_connected(void)
{
	connect_to_cloud();

	/* Long button press should trigger sending start and cloud poll */
	send_button_press_long();
	expect_storage_event(STORAGE_BATCH_REQUEST);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);
}

void test_threshold_reached_connected(void)
{
	connect_to_cloud();

	/* Threshold reached should trigger sending start and cloud poll */
	send_storage_threshold_reached();
	expect_storage_event(STORAGE_THRESHOLD_REACHED);
	expect_storage_event(STORAGE_BATCH_REQUEST);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);
}

void test_short_button_press_disconnected(void)
{
	/* Short button press should trigger sampling immediately */
	send_button_press_short();
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Complete location search */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
}

void test_ntn_fallback_pending(void)
{
	/* NTN fallback is only permitted to resume an existing cloud session, so establish one.
	 * Consume the echoed event so the later expect_no_events() does not flag it.
	 */
	send_cloud_session_established();
	expect_cloud_event(CLOUD_SESSION_ESTABLISHED);

	/* Trigger sampling to enter STATE_DISCONNECTED_SAMPLING with a location search in progress */
	send_button_press_short();
	expect_location_event(LOCATION_SEARCH_TRIGGER);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);

	/* TN search fails while the location search is still ongoing. The NTN fallback must be
	 * deferred until the search completes, so no NETWORK_CONNECT_NTN should be sent yet.
	 */
	send_network_tn_search_failed();
	expect_network_event(NETWORK_TN_SEARCH_FAILED);
	expect_no_events(2);

	/* Completing the location search should trigger the deferred NTN fallback */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_network_event(NETWORK_CONNECT_NTN);
}

void test_gnss_location_forwarded_intact(void)
{
	struct network_msg msg;
	int err;

	/* The network module requests a GNSS fix; main triggers a GNSS-only search. */
	send_network_gnss_location_req();
	expect_network_event(NETWORK_GNSS_LOCATION_REQ);
	expect_location_event(LOCATION_GNSS_SEARCH_TRIGGER);

	send_location_gnss_data(63.421000, 10.437000, 123.5f);

	err = wait_for_network_gnss_location(&msg, 5);
	TEST_ASSERT_EQUAL(0, err);

	TEST_ASSERT_FLOAT_WITHIN(0.0001f, 63.421000f, (float)msg.location.lat);
	TEST_ASSERT_FLOAT_WITHIN(0.0001f, 10.437000f, (float)msg.location.lon);
	TEST_ASSERT_FLOAT_WITHIN(0.0001f, 123.5f, msg.location.alt);
}

void test_ntn_fallback_immediate(void)
{
	/* NTN fallback is only permitted to resume an existing cloud session, so establish one. */
	send_cloud_session_established();

	/* When no location search is in progress (STATE_DISCONNECTED_WAITING), a TN search
	 * failure should trigger the NTN fallback immediately via the top-level handler.
	 */
	send_network_tn_search_failed();
	expect_network_event(NETWORK_TN_SEARCH_FAILED);
	expect_network_event(NETWORK_CONNECT_NTN);
}

void test_no_ntn_fallback_without_session(void)
{
	/* Without an established cloud session, a TN search failure must NOT fall back to NTN
	 * (a fresh connection over NTN is not permitted). The top-level handler instead schedules
	 * a TN retry via the reconnect back-off, so no immediate NETWORK_CONNECT_NTN is emitted.
	 */
	send_network_tn_search_failed();
	expect_network_event(NETWORK_TN_SEARCH_FAILED);
	expect_no_events(2);
}

void test_no_ntn_fallback_after_session_stopped(void)
{
	/* A session that has been torn down must clear the gate, so a later TN search failure
	 * does not fall back to NTN.
	 */
	send_cloud_session_established();
	send_cloud_session_stopped();
	k_sleep(K_MSEC(100));
	purge_all_events();

	send_network_tn_search_failed();
	expect_network_event(NETWORK_TN_SEARCH_FAILED);
	expect_no_events(2);
}

void test_no_ntn_fallback_without_tn_failure(void)
{
	/* A location search that completes without a preceding TN search failure must not
	 * trigger an NTN fallback.
	 */
	send_button_press_short();
	expect_location_event(LOCATION_SEARCH_TRIGGER);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);

	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_no_events(2);
}

void test_ntn_fallback_pending_cleared_on_new_cycle(void)
{
	/* NTN fallback is only permitted to resume an existing cloud session, so establish one. */
	send_cloud_session_established();

	/* Begin a sampling cycle and let a TN search failure mark the NTN fallback as pending */
	send_button_press_short();
	expect_location_event(LOCATION_SEARCH_TRIGGER);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);

	send_network_tn_search_failed();
	expect_network_event(NETWORK_TN_SEARCH_FAILED);

	/* Abort the cycle before the location search completes by connecting to cloud, then
	 * disconnect again. The pending fallback must not survive into the next cycle.
	 */
	send_cloud_connected();
	k_sleep(K_MSEC(500));
	purge_all_events();

	send_cloud_disconnected();
	expect_cloud_event(CLOUD_DISCONNECTED);

	/* Start a fresh sampling cycle. Completing the search must not trigger a stale NTN
	 * fallback left over from the aborted cycle.
	 */
	send_button_press_short();
	expect_location_event(LOCATION_SEARCH_TRIGGER);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);

	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_no_events(2);
}

/* The reconnect back-off starts at APP_RECONNECT_BACKOFF_INITIAL_SECONDS (60 s) in main.c. */
#define RECONNECT_BACKOFF_INITIAL_SECONDS 60

void test_ntn_reconnect_backoff_schedules_tn(void)
{
	int delay;

	/* An exhausted NTN attempt must schedule a retry that restarts from TN after the
	 * back-off elapses.
	 */
	send_network_ntn_search_failed();
	expect_network_event(NETWORK_NTN_SEARCH_FAILED);

	delay = wait_for_network_event(NETWORK_CONNECT_TN,
				       RECONNECT_BACKOFF_INITIAL_SECONDS * 2);
	TEST_ASSERT_GREATER_OR_EQUAL(0, delay);
}

/* Matches CONFIG_APP_TN_RECOVERY_INTERVAL_SECONDS set in CMakeLists.txt. */
#define TN_RECOVERY_INTERVAL_SECONDS 600

void test_tn_recovery_returns_to_tn(void)
{
	int delay;

	/* NTN is a fallback bearer only. While connected over NTN, the recovery timer must
	 * periodically request a disconnect so a TN search can be attempted.
	 */
	connect_to_cloud();

	send_network_connected_ntn();
	expect_network_event(NETWORK_CONNECTED_NTN);
	purge_all_events();

	delay = wait_for_network_event(NETWORK_DISCONNECT, TN_RECOVERY_INTERVAL_SECONDS * 2);
	TEST_ASSERT_GREATER_OR_EQUAL(0, delay);

	/* Once the network reports the disconnect, the TN search must start immediately,
	 * not after the reconnect back-off.
	 */
	send_network_disconnected();
	expect_network_event(NETWORK_DISCONNECTED);
	expect_network_event(NETWORK_CONNECT_TN);
}

void test_sampling_on_ntn_bounces_to_tn(void)
{
	/* GNSS cannot run while the modem is in NTN system mode, so a sampling cycle started
	 * while connected over NTN must bounce the bearer instead of triggering a location
	 * search that would fail.
	 */
	connect_to_cloud();
	send_network_connected_ntn();
	expect_network_event(NETWORK_CONNECTED_NTN);
	purge_all_events();

	send_button_press_short();

	/* Non-location sources are sampled in place; the bearer is bounced for the rest. */
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
	expect_network_event(NETWORK_DISCONNECT);

	/* Once the network reports the disconnect, the TN search starts immediately. */
	send_network_disconnected();
	expect_network_event(NETWORK_DISCONNECTED);
	expect_network_event(NETWORK_CONNECT_TN);

	/* TN is back: the deferred location sample is taken in place. */
	send_network_connected_tn();
	expect_network_event(NETWORK_CONNECTED_TN);
	expect_location_event(LOCATION_SEARCH_TRIGGER);
}

void test_sampling_on_ntn_falls_back_to_ntn_when_tn_unavailable(void)
{
	/* Same bounce, but TN is still gone: with a live cloud session the device must fall
	 * back to NTN (where the attach acquires a fresh GNSS fix as the location sample).
	 */
	send_cloud_session_established();
	expect_cloud_event(CLOUD_SESSION_ESTABLISHED);

	connect_to_cloud();
	send_network_connected_ntn();
	expect_network_event(NETWORK_CONNECTED_NTN);
	purge_all_events();

	send_button_press_short();
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
	expect_network_event(NETWORK_DISCONNECT);

	/* The cloud module pauses the session when the network drops. Mimic it so the state
	 * machine leaves the connected state (where search-failed events are ignored) before
	 * the TN search result arrives, as happens on hardware.
	 */
	send_cloud_disconnected();
	expect_cloud_event(CLOUD_DISCONNECTED);

	send_network_disconnected();
	expect_network_event(NETWORK_DISCONNECTED);
	expect_network_event(NETWORK_CONNECT_TN);

	send_network_tn_search_failed();
	expect_network_event(NETWORK_TN_SEARCH_FAILED);
	expect_network_event(NETWORK_CONNECT_NTN);
}

void test_tn_recovery_cancelled_on_tn_connect(void)
{
	/* Returning to TN must cancel the pending recovery, so no disconnect is requested
	 * after the recovery interval elapses.
	 */
	connect_to_cloud();

	send_network_connected_ntn();
	expect_network_event(NETWORK_CONNECTED_NTN);

	send_network_connected_tn();
	expect_network_event(NETWORK_CONNECTED_TN);
	purge_all_events();

	TEST_ASSERT_EQUAL(-ENOMSG, wait_for_network_event(NETWORK_DISCONNECT,
							  TN_RECOVERY_INTERVAL_SECONDS + 100));
}

void test_cloud_loss_schedules_tn_reconnect(void)
{
	int delay;

	/* Losing the cloud connection while connected must drive the connection lifecycle back up:
	 * schedule a TN search after the back-off so the (possibly paused) session can be resumed,
	 * falling back to NTN only if the TN search then fails.
	 */
	connect_to_cloud();
	purge_all_events();

	send_cloud_disconnected();
	expect_cloud_event(CLOUD_DISCONNECTED);

	delay = wait_for_network_event(NETWORK_CONNECT_TN,
				       RECONNECT_BACKOFF_INITIAL_SECONDS * 2);
	TEST_ASSERT_GREATER_OR_EQUAL(0, delay);
}

void test_ntn_reconnect_backoff_reset_on_connect(void)
{
	/* A pending reconnect must be cancelled when a connection is established, so that no
	 * NETWORK_CONNECT_TN is emitted after the back-off would otherwise have elapsed.
	 */
	send_network_ntn_search_failed();
	expect_network_event(NETWORK_NTN_SEARCH_FAILED);

	send_network_connected_tn();
	expect_network_event(NETWORK_CONNECTED_TN);

	/* Wait past the initial back-off; the cancelled retry must not fire. */
	expect_no_events(RECONNECT_BACKOFF_INITIAL_SECONDS + 5);
}

void test_ntn_reconnect_backoff_doubles(void)
{
	int first_delay;
	int second_delay;

	/* First failure retries after the initial back-off. */
	send_network_ntn_search_failed();
	expect_network_event(NETWORK_NTN_SEARCH_FAILED);
	first_delay = wait_for_network_event(NETWORK_CONNECT_TN,
					     RECONNECT_BACKOFF_INITIAL_SECONDS * 3);
	TEST_ASSERT_GREATER_OR_EQUAL(0, first_delay);

	/* Second consecutive failure must back off longer (exponential growth). */
	send_network_ntn_search_failed();
	expect_network_event(NETWORK_NTN_SEARCH_FAILED);
	second_delay = wait_for_network_event(NETWORK_CONNECT_TN,
					      RECONNECT_BACKOFF_INITIAL_SECONDS * 5);
	TEST_ASSERT_GREATER_OR_EQUAL(0, second_delay);

	TEST_ASSERT_GREATER_THAN(first_delay, second_delay);
}

void test_long_button_press_disconnected(void)
{
	/* Long button press when disconnected should be ignored */
	send_button_press_long();
	expect_no_events(500);
}

void test_threshold_reached_disconnected(void)
{
	/* Threshold reached when disconnected should be ignored */
	send_storage_threshold_reached();
	expect_storage_event(STORAGE_THRESHOLD_REACHED);
	expect_no_events(500);

	/* Connection after threshold reached should trigger immediate sending */
	connect_to_cloud();
	/* Expect events from connected_sending_entry -> cloud_send_now() */
	expect_storage_event(STORAGE_BATCH_REQUEST);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);

	/* Complete the send cycle to transition to STATE_CONNECTED_WAITING */
	send_storage_batch_close();
	expect_storage_event(STORAGE_BATCH_CLOSE);
}

void test_fota_downloading(void)
{
	connect_to_cloud();

	/* Transition to STATE_FOTA_DOWNLOADING */
	send_fota_msg(FOTA_DOWNLOADING_UPDATE);
	expect_fota_event(FOTA_DOWNLOADING_UPDATE);

	/* A cloud ready message and button trigger should now cause no action */
	send_cloud_connected();
	expect_cloud_event(CLOUD_CONNECTED);
	expect_no_events(7200);
	send_button_press_short();
	expect_no_events(7200);

	/* Cleanup */
	send_fota_msg(FOTA_DOWNLOAD_CANCELED);
	expect_fota_event(FOTA_DOWNLOAD_CANCELED);

	/* Then sampling resumes */
	expect_location_event(LOCATION_SEARCH_TRIGGER);
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
}

void test_fota_waiting_for_network_disconnect(void)
{
	connect_to_cloud();

	/* Transition to STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT */
	send_fota_msg(FOTA_DOWNLOADING_UPDATE);
	expect_fota_event(FOTA_DOWNLOADING_UPDATE);
	send_fota_msg(FOTA_SUCCESS_REBOOT_NEEDED);
	expect_fota_event(FOTA_SUCCESS_REBOOT_NEEDED);

	/* Veriy that the module sends NETWORK_DISCONNECT */
	expect_network_event(NETWORK_DISCONNECT);

	expect_no_events(10);

	/* Send a NETWORK_DISCONNECTED event to trigger transition to STATE_FOTA_REBOOTING */
	send_network_disconnected();
	expect_network_event(NETWORK_DISCONNECTED);
	send_cloud_disconnected();
	expect_cloud_event(CLOUD_DISCONNECTED);

	/* Verify that the module sends STORAGE_CLEAR before fota reboot */
	expect_storage_event(STORAGE_CLEAR);

	/* Give the system time to reboot */
	k_sleep(K_SECONDS(10));

	TEST_ASSERT_EQUAL(1, sys_reboot_fake.call_count);

	/* Cleanup */
	send_fota_msg(FOTA_DOWNLOAD_CANCELED);
	expect_fota_event(FOTA_DOWNLOAD_CANCELED);
}

void test_fota_waiting_for_network_disconnect_to_apply_image(void)
{
	connect_to_cloud();

	/* Transition to STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT_TO_APPLY_IMAGE */
	send_fota_msg(FOTA_DOWNLOADING_UPDATE);
	expect_fota_event(FOTA_DOWNLOADING_UPDATE);
	send_fota_msg(FOTA_IMAGE_APPLY_NEEDED);
	expect_fota_event(FOTA_IMAGE_APPLY_NEEDED);

	/* Veriy that the module sends NETWORK_DISCONNECT */
	expect_network_event(NETWORK_DISCONNECT);

	expect_no_events(10);

	/* Send a NETWORK_DISCONNECTED event to trigger transition to STATE_FOTA_APPLYING_IMAGE */
	send_network_disconnected();
	expect_network_event(NETWORK_DISCONNECTED);

	/* Verify that the module sends FOTA_IMAGE_APPLY */
	expect_fota_event(FOTA_IMAGE_APPLY);

	/* Trigger reboot */
	send_fota_msg(FOTA_SUCCESS_REBOOT_NEEDED);
	expect_fota_event(FOTA_SUCCESS_REBOOT_NEEDED);

	/* Verify that the module sends STORAGE_CLEAR before fota reboot */
	expect_storage_event(STORAGE_CLEAR);

	/* Give the system time to reboot */
	k_sleep(K_SECONDS(10));

	TEST_ASSERT_EQUAL(1, sys_reboot_fake.call_count);

	/* Cleanup */
	send_fota_msg(FOTA_DOWNLOAD_CANCELED);
	expect_fota_event(FOTA_DOWNLOAD_CANCELED);
}

void test_sensor_timer_multiple_expiries(void)
{
	connect_to_cloud();

	restart_sample_timer();

	/* Wait for sample timer to trigger sampling */
	k_sleep(K_SECONDS(CONFIG_APP_SAMPLING_INTERVAL_SECONDS));
	expect_timer_event(TIMER_EXPIRED_SAMPLE_DATA);
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Complete location search */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);

	/* Wait for next sample timer */
	k_sleep(K_SECONDS(CONFIG_APP_SAMPLING_INTERVAL_SECONDS));
	expect_timer_event(TIMER_EXPIRED_SAMPLE_DATA);
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Complete location search */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);
}

/* During network activity, no location search should be triggered */
void test_no_sampling_during_cloud_send(void)
{
	connect_to_cloud();

	/* Trigger cloud send */
	send_button_press_long();
	expect_storage_event(STORAGE_BATCH_REQUEST);
	expect_fota_event(FOTA_POLL_REQUEST);
	expect_cloud_event(CLOUD_SHADOW_GET_DELTA);

	/* Try to trigger sampling */
	send_button_press_short();

	/* Nothing should happen */
	expect_no_events(500);

	/* Close batch to signal send completion */
	send_storage_batch_close();
	expect_storage_event(STORAGE_BATCH_CLOSE);
}

void test_config_change(void)
{
	connect_to_cloud();

	/* Restart timer to know the timing for the next trigger */
	restart_sample_timer();

	/* Change sample interval and verify that the new interval is respected */
	config_change_sampling_interval(300);
	expect_cloud_event(CLOUD_SHADOW_RESPONSE_DELTA);
	expect_cloud_event(CLOUD_SHADOW_UPDATE_REPORTED_CONFIG);
	expect_timer_event(TIMER_CONFIG_CHANGED);

	k_sleep(K_SECONDS(300));
	expect_timer_event(TIMER_EXPIRED_SAMPLE_DATA);
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Complete location search */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);

	/* Restart timer to know the timing for the next trigger */
	restart_sample_timer();

	/* Change all parameters at once and verify that all changes are respected */
	config_change_all(200, 3);
	expect_storage_event(STORAGE_SET_THRESHOLD);
	expect_cloud_event(CLOUD_SHADOW_RESPONSE_DELTA);
	expect_cloud_event(CLOUD_SHADOW_UPDATE_REPORTED_CONFIG);
	expect_timer_event(TIMER_CONFIG_CHANGED);

	k_sleep(K_SECONDS(200));
	expect_timer_event(TIMER_EXPIRED_SAMPLE_DATA);
	expect_location_event(LOCATION_SEARCH_TRIGGER);

	/* Complete location search */
	send_location_search_done();
	expect_location_event(LOCATION_SEARCH_DONE);
	expect_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST);

	/* Close batch to signal send completion */
	send_storage_batch_close();
	expect_storage_event(STORAGE_BATCH_CLOSE);
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
