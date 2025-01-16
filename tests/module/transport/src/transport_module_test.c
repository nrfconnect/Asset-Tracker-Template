/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <unity.h>

#include <zephyr/fff.h>
#include "message_channel.h"
#include <zephyr/task_wdt/task_wdt.h>

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);
FAKE_VALUE_FUNC(int, nrf_cloud_client_id_get, char *, size_t);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_init);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_connect, const char * const);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_disconnect);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_shadow_device_status_update);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_bytes_send, uint8_t *, size_t, bool);

static K_SEM_DEFINE(cloud_disconnected, 0, 1);
static K_SEM_DEFINE(cloud_connected_ready, 0, 1);
static K_SEM_DEFINE(cloud_connected_paused, 0, 1);
static K_SEM_DEFINE(data_sent, 0, 1);
static K_SEM_DEFINE(fatal_error_received, 0, 1);

static void dummy_cb(const struct zbus_channel *chan)
{
	ARG_UNUSED(chan);
}

static void cloud_chan_cb(const struct zbus_channel *chan)
{
	if (chan == &CLOUD_CHAN) {
		enum cloud_msg_type status = *(enum cloud_msg_type *)chan->message;

		if (status == CLOUD_DISCONNECTED) {
			k_sem_give(&cloud_disconnected);
		} else if (status == CLOUD_CONNECTED_READY_TO_SEND) {
			k_sem_give(&cloud_connected_ready);
		} else if (status == CLOUD_CONNECTED_PAUSED) {
			k_sem_give(&cloud_connected_paused);
		}
	}
}

static void error_cb(const struct zbus_channel *chan)
{
	if (chan == &ERROR_CHAN) {
		enum error_type type = *(enum error_type *)chan->message;

		if (type == ERROR_FATAL) {
			k_sem_give(&fatal_error_received);
		}
	}
}

/* Define unused subscribers */
ZBUS_SUBSCRIBER_DEFINE(app, 1);
ZBUS_SUBSCRIBER_DEFINE(battery, 1);
ZBUS_SUBSCRIBER_DEFINE(environmental, 1);
ZBUS_SUBSCRIBER_DEFINE(fota, 1);
ZBUS_SUBSCRIBER_DEFINE(led, 1);
ZBUS_SUBSCRIBER_DEFINE(location, 1);
ZBUS_LISTENER_DEFINE(trigger, dummy_cb);
ZBUS_LISTENER_DEFINE(cloud, cloud_chan_cb);
ZBUS_LISTENER_DEFINE(error, error_cb);

void setUp(void)
{
	const struct zbus_channel *chan;

	/* Reset fakes */
	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(task_wdt_add);

	/* Clear all channels */
	zbus_sub_wait(&location, &chan, K_NO_WAIT);
	zbus_sub_wait(&app, &chan, K_NO_WAIT);
	zbus_sub_wait(&fota, &chan, K_NO_WAIT);
	zbus_sub_wait(&led, &chan, K_NO_WAIT);
	zbus_sub_wait(&battery, &chan, K_NO_WAIT);

	zbus_chan_add_obs(&CLOUD_CHAN, &cloud, K_NO_WAIT);
	zbus_chan_add_obs(&ERROR_CHAN, &error, K_NO_WAIT);
}

void test_initial_transition_to_disconnected(void)
{
	int err;

	err = k_sem_take(&cloud_disconnected, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
}

void test_transition_disconnected_connected_ready(void)
{
	int err;
	enum network_msg_type status = NETWORK_CONNECTED;

	zbus_chan_pub(&NETWORK_CHAN, &status, K_NO_WAIT);

	err = k_sem_take(&cloud_connected_ready, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
}

void test_sending_payload(void)
{
	struct payload payload = {
		.buffer = "test",
		.buffer_len = sizeof(payload.buffer) - 1,
	};

	zbus_chan_pub(&PAYLOAD_CHAN, &payload, K_NO_WAIT);

	/* Transport module needs CPU to run state machine */
	k_sleep(K_MSEC(100));

	TEST_ASSERT_EQUAL(1, nrf_cloud_coap_bytes_send_fake.call_count);
	TEST_ASSERT_EQUAL(0, strncmp(nrf_cloud_coap_bytes_send_fake.arg0_val,
				     payload.buffer, payload.buffer_len));
	TEST_ASSERT_EQUAL(payload.buffer_len, nrf_cloud_coap_bytes_send_fake.arg1_val);
}

void test_connected_ready_to_paused(void)
{
	int err;
	enum network_msg_type status = NETWORK_DISCONNECTED;

	zbus_chan_pub(&NETWORK_CHAN, &status, K_NO_WAIT);

	/* Transport module needs CPU to run state machine */
	k_sleep(K_MSEC(100));

	err = k_sem_take(&cloud_connected_paused, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
}

void test_connected_paused_to_ready_send_payload(void)
{
	int err;
	enum network_msg_type status = NETWORK_CONNECTED;
	struct payload payload = {
		.buffer = "Another test",
		.buffer_len = sizeof(payload.buffer) - 1,
	};

	/* Reset call count */
	nrf_cloud_coap_bytes_send_fake.call_count = 0;

	zbus_chan_pub(&NETWORK_CHAN, &status, K_NO_WAIT);

	/* Transport module needs CPU to run state machine */
	k_sleep(K_MSEC(100));

	err = k_sem_take(&cloud_connected_ready, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	zbus_chan_pub(&PAYLOAD_CHAN, &payload, K_NO_WAIT);

	/* Transport module needs CPU to run state machine */
	k_sleep(K_MSEC(100));

	TEST_ASSERT_EQUAL(1, nrf_cloud_coap_bytes_send_fake.call_count);
	TEST_ASSERT_EQUAL(0, strncmp(nrf_cloud_coap_bytes_send_fake.arg0_val,
				     payload.buffer, payload.buffer_len));
	TEST_ASSERT_EQUAL(payload.buffer_len, nrf_cloud_coap_bytes_send_fake.arg1_val);
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
