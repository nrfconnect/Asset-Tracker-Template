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
#include <zephyr/zbus/zbus.h>
#include <net/mqtt_helper.h>
#include <modem/modem_key_mgmt.h>
#include <nrf_modem.h>
#include <hw_id.h>

#include "cloud.h"
#include "network.h"
#include "location.h"
#include "fota.h"

DEFINE_FFF_GLOBALS;

/* Forward declarations - declare callback functions */
typedef void (*task_wdt_callback_t)(int, void *);
typedef void (*mqtt_helper_on_connack)(enum mqtt_conn_return_code, bool);
typedef void (*mqtt_helper_on_disconnect)(int);
typedef void (*mqtt_helper_on_publish)(struct mqtt_helper_buf, struct mqtt_helper_buf);
typedef void (*mqtt_helper_on_suback)(uint16_t, int);
typedef void (*mqtt_helper_on_puback)(uint16_t, int);

/* Forward declare custom fake functions */
static int mqtt_helper_init_custom_fake(struct mqtt_helper_cfg *cfg);

/* Function pointer variables for MQTT callbacks */
static mqtt_helper_on_connack on_connack;
static mqtt_helper_on_disconnect on_disconnect;
static mqtt_helper_on_publish on_publish;
static mqtt_helper_on_suback on_suback;
static mqtt_helper_on_puback on_puback;

ZBUS_CHAN_DEFINE(NETWORK_CHAN,
		 struct network_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.type = NETWORK_DISCONNECTED)
);

/* Mock functions */
FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);
FAKE_VALUE_FUNC(int, hw_id_get, char *, size_t);
FAKE_VALUE_FUNC(int, mqtt_helper_init, struct mqtt_helper_cfg *);
FAKE_VALUE_FUNC(int, mqtt_helper_connect, struct mqtt_helper_conn_params *);
FAKE_VALUE_FUNC(int, mqtt_helper_disconnect);
FAKE_VALUE_FUNC(int, mqtt_helper_publish, const struct mqtt_publish_param *);
FAKE_VALUE_FUNC(int, mqtt_helper_subscribe, struct mqtt_subscription_list *);
FAKE_VALUE_FUNC(int, modem_key_mgmt_write, nrf_sec_tag_t, enum modem_key_mgmt_cred_type, const void *, size_t);
FAKE_VALUE_FUNC(uint16_t, mqtt_helper_msg_id_get);

/* Forward declarations */
static void cloud_chan_cb(const struct zbus_channel *chan);

ZBUS_LISTENER_DEFINE(cloud_test_listener, cloud_chan_cb);

#define FAKE_DEVICE_ID "test_device"

static bool cloud_connected = false;

/* This is a custom fake function to capture the callback functions */
static int mqtt_helper_init_custom_fake(struct mqtt_helper_cfg *cfg)
{
	/* Store the callback function pointers for later use */
	on_connack = cfg->cb.on_connack;
	on_disconnect = cfg->cb.on_disconnect;
	on_publish = cfg->cb.on_publish;
	on_suback = cfg->cb.on_suback;
	on_puback = cfg->cb.on_puback;

	return 0;
}

static int hw_id_get_custom_fake(char *buf, size_t len)
{
	TEST_ASSERT(len >= sizeof(FAKE_DEVICE_ID));
	memcpy(buf, FAKE_DEVICE_ID, sizeof(FAKE_DEVICE_ID));

	return 0;
}

static void cloud_chan_cb(const struct zbus_channel *chan)
{
	if (chan == &CLOUD_CHAN) {
		const struct cloud_msg *cloud_msg = zbus_chan_const_msg(chan);
		enum cloud_msg_type status = cloud_msg->type;

		if (status == CLOUD_DISCONNECTED) {
			cloud_connected = false;
		} else if (status == CLOUD_CONNECTED) {
			cloud_connected = true;
		}
	}
}

static void network_disconnect(void)
{
	struct network_msg msg = {
		.type = NETWORK_DISCONNECTED
	};

	int err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Transport module needs CPU to run state machine */
	k_sleep(K_MSEC(100));
}

static void network_connect(void)
{
	struct network_msg msg = {
		.type = NETWORK_CONNECTED
	};

	int err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Transport module needs CPU to run state machine */
	k_sleep(K_MSEC(100));
}

static void publish_test_payload(void)
{
	struct cloud_msg msg = {
		.type = CLOUD_PAYLOAD_JSON,
		.payload.buffer = "{\"test\": 1}",
		.payload.buffer_data_len = strnlen(msg.payload.buffer, sizeof(msg.payload.buffer)),
	};

	int err = zbus_chan_pub(&CLOUD_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Transport module needs CPU to run state machine */
	k_sleep(K_MSEC(100));
}

void setUp(void)
{
	/* Reset fakes */
	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(task_wdt_add);
	RESET_FAKE(hw_id_get);
	RESET_FAKE(mqtt_helper_init);
	RESET_FAKE(mqtt_helper_connect);
	RESET_FAKE(mqtt_helper_disconnect);
	RESET_FAKE(mqtt_helper_publish);
	RESET_FAKE(mqtt_helper_subscribe);
	RESET_FAKE(modem_key_mgmt_write);

	hw_id_get_fake.custom_fake = hw_id_get_custom_fake;
	mqtt_helper_init_fake.custom_fake = mqtt_helper_init_custom_fake;

	zbus_chan_add_obs(&CLOUD_CHAN, &cloud_test_listener, K_NO_WAIT);
}

void tearDown(void)
{
	/* Clean up */
	network_disconnect();
	cloud_connected = false;
}

void test_should_call_mqtt_connect(void)
{
	/* Given */
	mqtt_helper_connect_fake.return_val = 0;

	/* When */
	network_connect();

	/* Expect */
	TEST_ASSERT_EQUAL(1, mqtt_helper_connect_fake.call_count);
}

void test_should_notify_connected(void)
{
	/* Given */
	mqtt_helper_subscribe_fake.return_val = 0;

	/* When */
	network_connect();
	on_connack(MQTT_CONNECTION_ACCEPTED, false);
	k_sleep(K_MSEC(100));

	/* Expect */
	TEST_ASSERT_TRUE(cloud_connected);
	TEST_ASSERT_EQUAL(1, mqtt_helper_subscribe_fake.call_count);
}

void test_should_publish_data_from_payload_channel(void)
{
	/* Given */
	mqtt_helper_publish_fake.return_val = 0;
	test_should_notify_connected();

	/* When */
	publish_test_payload();

	/* Expect */
	TEST_ASSERT_EQUAL(1, mqtt_helper_publish_fake.call_count);
}

void test_should_reconnect_on_network_disconnect(void)
{
	/* Given */
	mqtt_helper_connect_fake.return_val = 0;
	mqtt_helper_subscribe_fake.return_val = 0;

	/* When */
	network_connect();
	on_connack(MQTT_CONNECTION_ACCEPTED, false);
	k_sleep(K_MSEC(100));

	network_disconnect();
	k_sleep(K_MSEC(100));

	network_connect();
	on_connack(MQTT_CONNECTION_ACCEPTED, false);
	k_sleep(K_MSEC(100));

	/* Expect */
	TEST_ASSERT_EQUAL(2, mqtt_helper_connect_fake.call_count);
	TEST_ASSERT_EQUAL(2, mqtt_helper_subscribe_fake.call_count);
}

void test_should_not_publish_data_when_disconnected(void)
{
	/* Given */
	mqtt_helper_publish_fake.return_val = 0;
	test_should_notify_connected();

	/* When */
	network_disconnect();
	publish_test_payload();

	/* Expect */
	TEST_ASSERT_EQUAL(0, mqtt_helper_publish_fake.call_count);
}

void test_should_reconnect_linearly_when_network_is_connected(void)
{
	/* Given */
	mqtt_helper_connect_fake.return_val = 0;
	mqtt_helper_subscribe_fake.return_val = 0;

	/* When */
	network_connect();
	k_sleep(K_MINUTES(10));

	/* Expect */
	TEST_ASSERT_EQUAL(5, mqtt_helper_connect_fake.call_count);
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
