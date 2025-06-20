/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

 /* Ensure 'strnlen' is available even with -std=c99. */
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <unity.h>
#include <zephyr/fff.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/coap_client.h>
#include <zephyr/zbus/zbus.h>

#include "environmental.h"
#include "cloud.h"
#include "power.h"
#include "network.h"
#include "location.h"
#include "app_common.h"

DEFINE_FFF_GLOBALS;

/* Define the channels for testing */
ZBUS_CHAN_DEFINE(POWER_CHAN,
		 struct power_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);
ZBUS_CHAN_DEFINE(NETWORK_CHAN,
		 struct network_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.type = NETWORK_DISCONNECTED)
);
ZBUS_CHAN_DEFINE(ENVIRONMENTAL_CHAN,
		 struct environmental_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

ZBUS_CHAN_DEFINE(LOCATION_CHAN,
		 struct location_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);
FAKE_VALUE_FUNC(int, nrf_cloud_client_id_get, char *, size_t);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_init);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_connect, const char * const);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_disconnect);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_shadow_device_status_update);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_bytes_send, uint8_t *, size_t, bool);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_sensor_send, const char *, double, int64_t, bool);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_json_message_send, const char *, bool, bool);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_shadow_get, char *, size_t *, bool, enum coap_content_format);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_patch, const char *, const char *,
		const uint8_t *, size_t,
		enum coap_content_format, bool,
		coap_client_response_cb_t, void *);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_location_get,
		struct nrf_cloud_rest_location_request const *,
		struct nrf_cloud_location_result *const);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_agnss_data_get,
		struct nrf_cloud_rest_agnss_request const *,
		struct nrf_cloud_rest_agnss_result *);
FAKE_VOID_FUNC(location_cloud_location_ext_result_set, enum location_ext_result,
	       struct location_data *);
FAKE_VALUE_FUNC(int, location_agnss_data_process, const char *, size_t);

/* Forward declarations */
static void dummy_cb(const struct zbus_channel *chan);
static void cloud_chan_cb(const struct zbus_channel *chan);

/* Define unused subscribers */
ZBUS_SUBSCRIBER_DEFINE(app, 1);
ZBUS_SUBSCRIBER_DEFINE(battery, 1);
ZBUS_SUBSCRIBER_DEFINE(environmental, 1);
ZBUS_SUBSCRIBER_DEFINE(fota, 1);
ZBUS_SUBSCRIBER_DEFINE(led, 1);
ZBUS_SUBSCRIBER_DEFINE(location, 1);
ZBUS_LISTENER_DEFINE(trigger, dummy_cb);
ZBUS_LISTENER_DEFINE(cloud_test_listener, cloud_chan_cb);

#define FAKE_DEVICE_ID		"test_device"

static K_SEM_DEFINE(cloud_disconnected, 0, 1);
static K_SEM_DEFINE(cloud_connected, 0, 1);
static K_SEM_DEFINE(data_sent, 0, 1);

static int nrf_cloud_client_id_get_custom_fake(char *buf, size_t len)
{
	TEST_ASSERT(len >= sizeof(FAKE_DEVICE_ID));
	memcpy(buf, FAKE_DEVICE_ID, sizeof(FAKE_DEVICE_ID));

	return 0;
}

static void dummy_cb(const struct zbus_channel *chan)
{
	ARG_UNUSED(chan);
}

static void cloud_chan_cb(const struct zbus_channel *chan)
{
	if (chan == &CLOUD_CHAN) {
		const struct cloud_msg *cloud_msg = zbus_chan_const_msg(chan);
		enum cloud_msg_type status = cloud_msg->type;

		if (status == CLOUD_DISCONNECTED) {
			k_sem_give(&cloud_disconnected);
		} else if (status == CLOUD_CONNECTED) {
			k_sem_give(&cloud_connected);
		}
	}
}

/* Static observer node for zbus */
static struct zbus_observer_node obs_node;

void setUp(void)
{
	const struct zbus_channel *chan;

	/* Reset fakes */
	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(task_wdt_add);
	RESET_FAKE(nrf_cloud_client_id_get);
	RESET_FAKE(nrf_cloud_coap_json_message_send);
	RESET_FAKE(nrf_cloud_coap_connect);

	nrf_cloud_client_id_get_fake.custom_fake = nrf_cloud_client_id_get_custom_fake;

	/* Clear all channels */
	zbus_sub_wait(&location, &chan, K_NO_WAIT);
	zbus_sub_wait(&app, &chan, K_NO_WAIT);
	zbus_sub_wait(&fota, &chan, K_NO_WAIT);
	zbus_sub_wait(&led, &chan, K_NO_WAIT);
	zbus_sub_wait(&battery, &chan, K_NO_WAIT);

	zbus_chan_add_obs(&CLOUD_CHAN, &cloud_test_listener, &obs_node, K_NO_WAIT);
}

void test_initial_transition_to_disconnected(void)
{
	int err;

	err = k_sem_take(&cloud_disconnected, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
}

void test_connecting_backoff(void)
{
	int err;
	struct network_msg msg = { .type = NETWORK_CONNECTED, };
	uint64_t connect_start_time;
	uint64_t connect_duration_sec;

	nrf_cloud_coap_connect_fake.return_val = -EAGAIN;
	connect_start_time = k_uptime_get();

	msg.type = NETWORK_CONNECTED;

	err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Transport module needs CPU to run state machine */
	k_sleep(K_MSEC(10));

	err = k_sem_take(&cloud_connected, K_SECONDS(60));
	TEST_ASSERT_EQUAL(-EAGAIN, err);

	connect_duration_sec = k_uptime_delta(&connect_start_time) / MSEC_PER_SEC;

	/* Check that the connection attempt took at least
	 * CONFIG_APP_CLOUD_BACKOFF_INITIAL_SECONDS
	 */

	TEST_ASSERT_GREATER_OR_EQUAL(CONFIG_APP_CLOUD_BACKOFF_INITIAL_SECONDS,
				     connect_duration_sec);
}

void test_transition_disconnected_connected_ready(void)
{
	int err;
	enum network_msg_type status = NETWORK_CONNECTED;

	zbus_chan_pub(&NETWORK_CHAN, &status, K_NO_WAIT);

	err = k_sem_take(&cloud_connected, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
}

void test_sending_payload(void)
{
	int err;
	struct cloud_msg msg = {
		.type = CLOUD_PAYLOAD_JSON,
		.payload.buffer = "{\"test\": 1}",
		.payload.buffer_data_len = strnlen(msg.payload.buffer, sizeof(msg.payload.buffer)),
	};

	err = zbus_chan_pub(&CLOUD_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Transport module needs CPU to run state machine */
	k_sleep(K_MSEC(100));

	TEST_ASSERT_EQUAL(1, nrf_cloud_coap_json_message_send_fake.call_count);
	TEST_ASSERT_EQUAL(0, strncmp(nrf_cloud_coap_json_message_send_fake.arg0_val,
				     msg.payload.buffer, msg.payload.buffer_data_len));
	TEST_ASSERT_EQUAL(false, nrf_cloud_coap_json_message_send_fake.arg1_val);
	TEST_ASSERT_EQUAL(false, nrf_cloud_coap_json_message_send_fake.arg2_val);
}

void test_connected_to_disconnected(void)
{
	int err;
	enum network_msg_type status = NETWORK_DISCONNECTED;

	zbus_chan_pub(&NETWORK_CHAN, &status, K_NO_WAIT);

	/* Transport module needs CPU to run state machine */
	k_sleep(K_MSEC(100));

	err = k_sem_take(&cloud_disconnected, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
}

void test_connected_disconnected_to_connected_send_payload(void)
{
	int err;
	enum network_msg_type status = NETWORK_CONNECTED;
	struct cloud_msg msg = {
		.type = CLOUD_PAYLOAD_JSON,
		.payload.buffer = "{\"Another\": \"1\"}",
		.payload.buffer_data_len = strnlen(msg.payload.buffer, sizeof(msg.payload.buffer)),
	};

	/* Reset call count */
	nrf_cloud_coap_bytes_send_fake.call_count = 0;

	err = zbus_chan_pub(&NETWORK_CHAN, &status, K_NO_WAIT);
	TEST_ASSERT_EQUAL(0, err);

	/* Transport module needs CPU to run state machine */
	k_sleep(K_MSEC(100));

	err = k_sem_take(&cloud_connected, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	err = zbus_chan_pub(&CLOUD_CHAN, &msg, K_NO_WAIT);
	TEST_ASSERT_EQUAL(0, err);

	/* Transport module needs CPU to run state machine */
	k_sleep(K_MSEC(100));

	TEST_ASSERT_EQUAL(1, nrf_cloud_coap_json_message_send_fake.call_count);
	TEST_ASSERT_EQUAL(0, strncmp(nrf_cloud_coap_json_message_send_fake.arg0_val,
				     msg.payload.buffer, msg.payload.buffer_data_len));
	TEST_ASSERT_EQUAL(false, nrf_cloud_coap_json_message_send_fake.arg1_val);
	TEST_ASSERT_EQUAL(false, nrf_cloud_coap_json_message_send_fake.arg2_val);
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
