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
#include <zephyr/kernel.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/coap_client.h>
#include <net/nrf_provisioning.h>
#include <net/nrf_cloud_rest.h>
#include <modem/modem_attest_token.h>
#include <zephyr/zbus/zbus.h>

#include "environmental.h"
#include "cloud.h"
#include "power.h"
#include "network.h"
#include "location.h"
#include "storage.h"
#include "storage_data_types.h"
#include "app_common.h"

DEFINE_FFF_GLOBALS;

#define FAKE_DEVICE_ID			"test_device"
#define WAIT_TIMEOUT			2
#define CONNECT_RETRY_TIMEOUT_SEC	60
#define PROCESSING_DELAY_MS		500
#define INITIAL_PROVISIONING_RETRY_SEC	6
#define SECOND_PROVISIONING_RETRY_SEC	12
#define FAKE_TOKEN			"fake_token"
#define FAKE_TOKEN_SIZE			(sizeof(FAKE_TOKEN) - 1)

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

/* Define storage channels used by the cloud module */
ZBUS_CHAN_DEFINE(STORAGE_CHAN,
		 struct storage_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.type = STORAGE_MODE_PASSTHROUGH)
);

ZBUS_CHAN_DEFINE(STORAGE_DATA_CHAN,
		 struct storage_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.type = STORAGE_DATA)
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
FAKE_VALUE_FUNC(int, nrf_cloud_coap_location_send, const struct nrf_cloud_gnss_data *, bool);
FAKE_VALUE_FUNC(int, date_time_now, int64_t *);
FAKE_VOID_FUNC(location_cloud_location_ext_result_set, enum location_ext_result,
	       struct location_data *);
FAKE_VALUE_FUNC(int, location_agnss_data_process, const char *, size_t);
FAKE_VALUE_FUNC(int, nrf_provisioning_init, nrf_provisioning_event_cb_t);
FAKE_VALUE_FUNC(int, nrf_provisioning_trigger_manually);
FAKE_VALUE_FUNC(int, storage_batch_read, struct storage_data_item *, k_timeout_t);

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
ZBUS_SUBSCRIBER_DEFINE(cloud_test_sub, 16);
ZBUS_LISTENER_DEFINE(trigger, dummy_cb);
ZBUS_LISTENER_DEFINE(cloud_test_listener, cloud_chan_cb);

/* Attach a simple listener to storage channels to ensure observers exist */
ZBUS_CHAN_ADD_OBS(STORAGE_CHAN, cloud_test_listener, 0);
ZBUS_CHAN_ADD_OBS(STORAGE_DATA_CHAN, cloud_test_listener, 0);
ZBUS_CHAN_ADD_OBS(CLOUD_CHAN, cloud_test_sub, 0);

static K_SEM_DEFINE(cloud_disconnected, 0, 1);
static K_SEM_DEFINE(cloud_connected, 0, 1);
static K_SEM_DEFINE(data_sent, 0, 1);
static K_SEM_DEFINE(cloud_module_response_recv_sem, 0, 1);
static struct cloud_msg last_shadow_response;

static nrf_provisioning_event_cb_t handler;

/* Custom fake for storage_batch_read to drive batch processing in cloud module */
enum fake_batch_mode {
	FAKE_BATCH_NONE = 0,
	FAKE_BATCH_BATTERY,
	FAKE_BATCH_ENV,
	FAKE_BATCH_NET,
};

static enum fake_batch_mode fake_mode;
static int fake_read_calls;

static int storage_batch_read_custom(struct storage_data_item *out_item, k_timeout_t timeout)
{
	ARG_UNUSED(timeout);

	if (fake_mode == FAKE_BATCH_NONE) {
		return -EAGAIN;
	}

	if (fake_read_calls++ == 0) {
		switch (fake_mode) {
		case FAKE_BATCH_BATTERY:
			out_item->type = STORAGE_TYPE_BATTERY;
			out_item->data.BATTERY = 87.5;
			break;
		case FAKE_BATCH_ENV:
			out_item->type = STORAGE_TYPE_ENVIRONMENTAL;
			out_item->data.ENVIRONMENTAL.temperature = 21.5;
			out_item->data.ENVIRONMENTAL.humidity = 40.0;
			out_item->data.ENVIRONMENTAL.pressure = 1002.3;
			break;
		case FAKE_BATCH_NET:
			out_item->type = STORAGE_TYPE_NETWORK;
			out_item->data.NETWORK.type = NETWORK_QUALITY_SAMPLE_RESPONSE;
			out_item->data.NETWORK.conn_eval_params.energy_estimate = 5;
			out_item->data.NETWORK.conn_eval_params.rsrp = -96;
			break;
		default:
			return -EAGAIN;
		}

		return 0;
	}

	return -EAGAIN;
}

static int nrf_cloud_client_id_get_custom_fake(char *buf, size_t len)
{
	TEST_ASSERT(len >= sizeof(FAKE_DEVICE_ID));
	memcpy(buf, FAKE_DEVICE_ID, sizeof(FAKE_DEVICE_ID));

	return 0;
}

static int nrf_provisioning_init_custom_fake(nrf_provisioning_event_cb_t cb)
{
	handler = cb;

	return 0;
}

static void connect_cloud(void)
{
	int err;
	struct network_msg nw = { .type = NETWORK_CONNECTED };

	err = zbus_chan_pub(&NETWORK_CHAN, &nw, K_NO_WAIT);
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_MSEC(PROCESSING_DELAY_MS));
}

static void setup_cloud_and_passthrough(void)
{
	int err;
	struct network_msg network_msg = {
		.type = NETWORK_CONNECTED
	};
	struct storage_msg passthrough_msg = {
		.type = STORAGE_MODE_PASSTHROUGH
	};

	/* Connect to cloud */
	zbus_chan_pub(&NETWORK_CHAN, &network_msg, K_NO_WAIT);
	err = k_sem_take(&cloud_connected, K_SECONDS(WAIT_TIMEOUT));
	TEST_ASSERT_EQUAL(0, err);

	/* Enable passthrough mode */
	err = zbus_chan_pub(&STORAGE_CHAN, &passthrough_msg, K_NO_WAIT);
	TEST_ASSERT_EQUAL(0, err);
	k_sleep(K_MSEC(PROCESSING_DELAY_MS));
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
		} else if (status == CLOUD_SHADOW_RESPONSE_EMPTY_DESIRED ||
			   status == CLOUD_SHADOW_RESPONSE_EMPTY_DELTA ||
			   status == CLOUD_SHADOW_RESPONSE_DESIRED ||
			   status == CLOUD_SHADOW_GET_DESIRED ||
			   status == CLOUD_SHADOW_GET_DELTA ||
			   status == CLOUD_SHADOW_RESPONSE_DELTA) {
			memcpy(&last_shadow_response, cloud_msg, sizeof(struct cloud_msg));
			k_sem_give(&cloud_module_response_recv_sem);

			printk("cloud_chan_cb: received cloud message of type %d\n", status);
		}
	}
}

static int nrf_cloud_coap_shadow_get_empty_fake(char *buffer, size_t *buffer_data_len,
						 bool delta, enum coap_content_format format)
{
	ARG_UNUSED(buffer);
	ARG_UNUSED(delta);
	ARG_UNUSED(format);

	/* Simulate empty desired section by setting buffer_data_len to 0 */
	*buffer_data_len = 0;
	return 0;
}

static int nrf_cloud_coap_shadow_get_response_fake(char *buffer, size_t *buffer_data_len,
							bool delta, enum coap_content_format format)
{
	ARG_UNUSED(delta);
	ARG_UNUSED(format);

	static const uint8_t test_payload[] = { 0xDE, 0xAD, 0xBE, 0xEF };

	TEST_ASSERT(buffer != NULL);
	TEST_ASSERT(buffer_data_len != NULL);
	TEST_ASSERT(*buffer_data_len >= sizeof(test_payload));

	memcpy(buffer, test_payload, sizeof(test_payload));
	*buffer_data_len = sizeof(test_payload);

	return 0;
}

/* Common timing helpers */
static void wait_for_processing(void)
{
	k_sleep(K_MSEC(PROCESSING_DELAY_MS));
}

/* Helper for publishing messages to a channel */
static void publish_and_assert(const struct zbus_channel *chan, const void *msg)
{
	int err;

	err = zbus_chan_pub(chan, msg, K_NO_WAIT);
	TEST_ASSERT_EQUAL(0, err);
}

/* Helper for consuming a message from subscriber queue */
static void consume_message_and_assert_channel(const struct zbus_channel **chan,
					       k_timeout_t timeout)
{
	int err;

	err = zbus_sub_wait(&cloud_test_sub, chan, timeout);
	TEST_ASSERT_EQUAL(0, err);
	TEST_ASSERT_EQUAL_PTR(&CLOUD_CHAN, *chan);
}

/* Helper to wait for and verify a specific cloud message type */
static void wait_for_cloud_message(enum cloud_msg_type expected_type, k_timeout_t timeout)
{
	const struct zbus_channel *chan;
	const struct cloud_msg *msg;

	consume_message_and_assert_channel(&chan, timeout);
	msg = zbus_chan_const_msg(chan);
	TEST_ASSERT_EQUAL(expected_type, msg->type);
}

/* Helper to wait for cloud connected semaphore */
static void wait_for_cloud_connected(k_timeout_t timeout)
{
	int err;

	err = k_sem_take(&cloud_connected, timeout);
	TEST_ASSERT_EQUAL(0, err);
}

/* Helper to wait for cloud disconnected semaphore */
static void wait_for_cloud_disconnected(k_timeout_t timeout)
{
	int err;

	err = k_sem_take(&cloud_disconnected, timeout);
	TEST_ASSERT_EQUAL(0, err);
}

void setUp(void)
{
	const struct zbus_channel *chan;

	/* Reset fakes */
	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(task_wdt_add);
	RESET_FAKE(nrf_cloud_client_id_get);
	RESET_FAKE(nrf_cloud_coap_init);
	RESET_FAKE(nrf_cloud_coap_connect);
	RESET_FAKE(nrf_cloud_coap_disconnect);
	RESET_FAKE(nrf_cloud_coap_json_message_send);
	RESET_FAKE(nrf_cloud_coap_location_send);
	RESET_FAKE(nrf_cloud_coap_shadow_get);
	RESET_FAKE(nrf_cloud_coap_patch);
	RESET_FAKE(date_time_now);
	RESET_FAKE(nrf_provisioning_init);
	RESET_FAKE(nrf_provisioning_trigger_manually);
	RESET_FAKE(storage_batch_read);
	RESET_FAKE(nrf_cloud_coap_sensor_send);
	RESET_FAKE(nrf_cloud_coap_location_get);

	nrf_cloud_client_id_get_fake.custom_fake = nrf_cloud_client_id_get_custom_fake;
	nrf_provisioning_init_fake.custom_fake = nrf_provisioning_init_custom_fake;

	k_sem_reset(&cloud_disconnected);
	k_sem_reset(&cloud_connected);
	k_sem_reset(&data_sent);
	k_sem_reset(&cloud_module_response_recv_sem);

	/* Set default return values */
	nrf_cloud_coap_location_send_fake.return_val = 0;
	storage_batch_read_fake.return_val = -EAGAIN;

	/* Clear all channels */
	zbus_sub_wait(&location, &chan, K_NO_WAIT);
	zbus_sub_wait(&app, &chan, K_NO_WAIT);
	zbus_sub_wait(&fota, &chan, K_NO_WAIT);
	zbus_sub_wait(&led, &chan, K_NO_WAIT);
	zbus_sub_wait(&battery, &chan, K_NO_WAIT);

	/* Drain cloud_test_sub queue from any messages from previous tests */
	while (zbus_sub_wait(&cloud_test_sub, &chan, K_NO_WAIT) == 0) {
	}

	zbus_chan_add_obs(&CLOUD_CHAN, &cloud_test_listener, K_NO_WAIT);
}

void tearDown(void)
{
	struct network_msg msg = {
		.type = NETWORK_DISCONNECTED
	};

	zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));

	/* Give cloud module time to process disconnect and reach stable state.
	 * Need extra time as cloud transitions through CONNECTED_PAUSED -> DISCONNECTED
	 */
	k_sleep(K_SECONDS(8));

	/* Reset semaphores to clear any pending signals */
	k_sem_reset(&cloud_connected);
	k_sem_reset(&cloud_disconnected);
}

void test_should_initially_transition_to_disconnected(void)
{
	int err;

	err = k_sem_take(&cloud_disconnected, K_SECONDS(WAIT_TIMEOUT));
	TEST_ASSERT_EQUAL(0, err);
}

void test_should_handle_provisioning_when_device_not_claimed(void)
{
	struct network_msg network_msg = {
		.type = NETWORK_CONNECTED
	};
	struct nrf_provisioning_callback_data event = {
		.type = NRF_PROVISIONING_EVENT_FAILED
	};
	struct nrf_attestation_token token = {
		.attest = FAKE_TOKEN,
		.attest_sz = FAKE_TOKEN_SIZE,
		.cose = FAKE_TOKEN,
		.cose_sz = FAKE_TOKEN_SIZE,
	};

	event.token = &token;

	nrf_cloud_coap_connect_fake.return_val = -EACCES;

	publish_and_assert(&NETWORK_CHAN, &network_msg);
	wait_for_processing();

	TEST_ASSERT_EQUAL(1, nrf_cloud_coap_connect_fake.call_count);
	TEST_ASSERT_EQUAL(1, nrf_provisioning_trigger_manually_fake.call_count);

	handler(&event);

	k_sleep(K_SECONDS(INITIAL_PROVISIONING_RETRY_SEC + 1));

	TEST_ASSERT_EQUAL(2, nrf_provisioning_trigger_manually_fake.call_count);

	event.type = NRF_PROVISIONING_EVENT_FAILED_DEVICE_NOT_CLAIMED;

	handler(&event);

	k_sleep(K_SECONDS(SECOND_PROVISIONING_RETRY_SEC + 1));

	TEST_ASSERT_EQUAL(3, nrf_provisioning_trigger_manually_fake.call_count);

	event.type = NRF_PROVISIONING_EVENT_DONE;

	nrf_cloud_coap_connect_fake.return_val = 0;

	handler(&event);

	wait_for_cloud_connected(K_SECONDS(WAIT_TIMEOUT));
}

/* Test provisioning flow when no commands are received from the server */
void test_should_handle_provisioning_when_no_commands_received(void)
{
	struct network_msg network_msg = {
		.type = NETWORK_CONNECTED
	};
	struct cloud_msg cloud_msg = {
		.type = CLOUD_PROVISIONING_REQUEST
	};
	struct nrf_provisioning_callback_data event = {
		.type = NRF_PROVISIONING_EVENT_NO_COMMANDS
	};

	publish_and_assert(&NETWORK_CHAN, &network_msg);
	wait_for_processing();
	wait_for_cloud_connected(K_SECONDS(WAIT_TIMEOUT));

	publish_and_assert(&CLOUD_CHAN, &cloud_msg);
	wait_for_processing();
	wait_for_cloud_disconnected(K_SECONDS(WAIT_TIMEOUT));

	wait_for_processing();

	TEST_ASSERT_EQUAL(1, nrf_provisioning_trigger_manually_fake.call_count);

	handler(&event);

	wait_for_processing();

	TEST_ASSERT_EQUAL(1, nrf_cloud_coap_connect_fake.call_count);

	wait_for_cloud_connected(K_SECONDS(WAIT_TIMEOUT));
}

void test_should_transition_from_disconnected_to_connected_ready(void)
{
	struct network_msg msg = {
		.type = NETWORK_CONNECTED
	};

	publish_and_assert(&NETWORK_CHAN, &msg);
	wait_for_cloud_connected(K_SECONDS(WAIT_TIMEOUT));
	wait_for_processing();
}

void test_should_handle_provisioning_request_from_connected_state(void)
{
	struct cloud_msg provisioning_msg = {
		.type = CLOUD_PROVISIONING_REQUEST
	};
	struct nrf_provisioning_callback_data event = {
		.type = NRF_PROVISIONING_EVENT_DONE
	};

	test_should_transition_from_disconnected_to_connected_ready();

	publish_and_assert(&CLOUD_CHAN, &provisioning_msg);
	wait_for_processing();

	TEST_ASSERT_EQUAL(1, nrf_provisioning_trigger_manually_fake.call_count);

	wait_for_cloud_disconnected(K_SECONDS(WAIT_TIMEOUT));

	handler(&event);

	wait_for_cloud_connected(K_SECONDS(WAIT_TIMEOUT));
}

void test_should_backoff_on_connection_failure(void)
{
	int err;
	uint64_t connect_start_time;
	uint64_t connect_duration_sec;

	nrf_cloud_coap_connect_fake.return_val = -EAGAIN;
	connect_start_time = k_uptime_get();

	test_should_transition_from_disconnected_to_connected_ready();

	k_sleep(K_MSEC(PROCESSING_DELAY_MS / 10));

	err = k_sem_take(&cloud_connected, K_SECONDS(CONNECT_RETRY_TIMEOUT_SEC));
	TEST_ASSERT_EQUAL(-EAGAIN, err);

	connect_duration_sec = k_uptime_delta(&connect_start_time) / MSEC_PER_SEC;

	TEST_ASSERT_GREATER_OR_EQUAL(CONFIG_APP_CLOUD_BACKOFF_INITIAL_SECONDS,
				     connect_duration_sec);
}

void test_should_send_json_payload_to_cloud(void)
{
	struct cloud_msg msg = {
		.type = CLOUD_PAYLOAD_JSON,
		.payload.buffer = "{\"test\": 1}",
		.payload.buffer_data_len = strnlen(msg.payload.buffer, sizeof(msg.payload.buffer)),
	};

	test_should_transition_from_disconnected_to_connected_ready();

	publish_and_assert(&CLOUD_CHAN, &msg);
	wait_for_processing();

	TEST_ASSERT_EQUAL(1, nrf_cloud_coap_json_message_send_fake.call_count);
	TEST_ASSERT_EQUAL(0, strncmp(nrf_cloud_coap_json_message_send_fake.arg0_val,
				     msg.payload.buffer, msg.payload.buffer_data_len));
	TEST_ASSERT_EQUAL(false, nrf_cloud_coap_json_message_send_fake.arg1_val);
	TEST_ASSERT_EQUAL(false, nrf_cloud_coap_json_message_send_fake.arg2_val);
}

void test_connected_to_disconnected(void)
{
	struct network_msg msg = {
		.type = NETWORK_CONNECTED
	};

	publish_and_assert(&NETWORK_CHAN, &msg);
	wait_for_cloud_connected(K_SECONDS(1));

	msg.type = NETWORK_DISCONNECTED;

	publish_and_assert(&NETWORK_CHAN, &msg);
	wait_for_cloud_disconnected(K_SECONDS(1));
}

void test_connected_disconnected_to_connected_send_payload_disconnect(void)
{
	struct network_msg network_msg = {
		.type = NETWORK_CONNECTED
	};
	struct cloud_msg msg = {
		.type = CLOUD_PAYLOAD_JSON,
		.payload.buffer = "{\"Another\": \"1\"}",
		.payload.buffer_data_len = strnlen(msg.payload.buffer, sizeof(msg.payload.buffer)),
	};

	/* Reset call count */
	nrf_cloud_coap_bytes_send_fake.call_count = 0;

	publish_and_assert(&NETWORK_CHAN, &network_msg);
	wait_for_processing();
	wait_for_cloud_connected(K_SECONDS(1));

	publish_and_assert(&CLOUD_CHAN, &msg);
	wait_for_processing();

	TEST_ASSERT_EQUAL(1, nrf_cloud_coap_json_message_send_fake.call_count);
	TEST_ASSERT_EQUAL(0, strncmp(nrf_cloud_coap_json_message_send_fake.arg0_val,
				     msg.payload.buffer, msg.payload.buffer_data_len));
	TEST_ASSERT_EQUAL(false, nrf_cloud_coap_json_message_send_fake.arg1_val);
	TEST_ASSERT_EQUAL(false, nrf_cloud_coap_json_message_send_fake.arg2_val);

	network_msg.type = NETWORK_DISCONNECTED;

	publish_and_assert(&NETWORK_CHAN, &network_msg);
	wait_for_processing();
	wait_for_cloud_disconnected(K_SECONDS(1));
}

/* Test GNSS location data handling via storage module in passthrough mode */
void test_gnss_location_data_handling(void)
{
	struct network_msg network_msg = {
		.type = NETWORK_CONNECTED
	};
	struct storage_msg passthrough_msg = {
		.type = STORAGE_MODE_PASSTHROUGH
	};
	struct location_data mock_location = {
		.latitude = 63.421,
		.longitude = 10.437,
		.accuracy = 5.0,
		.datetime.valid = true,
		.datetime.year = 2025,
		.datetime.month = 1,
		.datetime.day = 15,
		.datetime.hour = 12,
		.datetime.minute = 30,
		.datetime.second = 45,
		.datetime.ms = 0
	};
	struct location_msg location_msg = {
		.type = LOCATION_GNSS_DATA,
		.gnss_data = mock_location
	};
	struct storage_msg storage_data_msg = {
		.type = STORAGE_DATA,
		.data_type = STORAGE_TYPE_LOCATION,
		.data_len = sizeof(struct location_msg)
	};

	/* Copy location message into storage message buffer */
	memcpy(storage_data_msg.buffer, &location_msg, sizeof(location_msg));

	/* Connect to cloud */
	publish_and_assert(&NETWORK_CHAN, &network_msg);
	wait_for_cloud_connected(K_SECONDS(1));
	publish_and_assert(&STORAGE_CHAN, &passthrough_msg);

	/* Give the module time to process mode change */
	k_sleep(K_MSEC(10));

	/* Send GNSS location data via storage data channel (passthrough mode) */
	publish_and_assert(&STORAGE_DATA_CHAN, &storage_data_msg);
	wait_for_processing();

	/* Verify that GNSS location data was sent to nRF Cloud */
	TEST_ASSERT_EQUAL(1, nrf_cloud_coap_location_send_fake.call_count);

	/* Basic verification that the function was called with valid arguments */
	if (nrf_cloud_coap_location_send_fake.call_count > 0) {
		TEST_ASSERT_NOT_NULL(nrf_cloud_coap_location_send_fake.arg0_val);
	}
}

void test_storage_data_battery_sent_to_cloud(void)
{
	struct storage_msg batch_available = {
		.type = STORAGE_BATCH_AVAILABLE,
		.data_len = 1,
		.session_id = 0xAABBCCDD,
		.more_data = false,
	};

	connect_cloud();

	/* Prepare fake to return one battery item, then -EAGAIN */
	fake_mode = FAKE_BATCH_BATTERY;
	fake_read_calls = 0;
	storage_batch_read_fake.custom_fake = storage_batch_read_custom;

	publish_and_assert(&STORAGE_CHAN, &batch_available);
	wait_for_processing();

	/* One successful read + one -EAGAIN drain */
	TEST_ASSERT_EQUAL(2, storage_batch_read_fake.call_count);
	TEST_ASSERT_EQUAL(1, nrf_cloud_coap_sensor_send_fake.call_count);
}

void test_storage_data_environmental_sent_to_cloud(void)
{
	struct storage_msg batch_available = {
		.type = STORAGE_BATCH_AVAILABLE,
		.data_len = 1,
		.session_id = 0x11223344,
		.more_data = false,
	};

	connect_cloud();

	/* Prepare fake to return one environmental item, then -EAGAIN */
	fake_mode = FAKE_BATCH_ENV;
	fake_read_calls = 0;
	storage_batch_read_fake.custom_fake = storage_batch_read_custom;

	publish_and_assert(&STORAGE_CHAN, &batch_available);
	wait_for_processing();

	/* One successful read + one -EAGAIN drain */
	TEST_ASSERT_EQUAL(2, storage_batch_read_fake.call_count);
	TEST_ASSERT_EQUAL(3, nrf_cloud_coap_sensor_send_fake.call_count);
}

void test_storage_data_network_conn_eval_sent_to_cloud(void)
{
	struct storage_msg batch_available = {
		.type = STORAGE_BATCH_AVAILABLE,
		.data_len = 1,
		.session_id = 0x55667788,
		.more_data = false,
	};

	connect_cloud();

	/* Prepare fake to return one network item, then -EAGAIN */
	fake_mode = FAKE_BATCH_NET;
	fake_read_calls = 0;
	storage_batch_read_fake.custom_fake = storage_batch_read_custom;

	publish_and_assert(&STORAGE_CHAN, &batch_available);
	wait_for_processing();

	/* One successful read + one -EAGAIN drain */
	TEST_ASSERT_EQUAL(2, storage_batch_read_fake.call_count);
	/* Expect two sensor publishes: CONEVAL and RSRP */
	TEST_ASSERT_EQUAL(2, nrf_cloud_coap_sensor_send_fake.call_count);
}

void test_provisioning_failed_with_network_connected_should_go_to_backoff(void)
{
	struct network_msg network_msg = {
		.type = NETWORK_CONNECTED
	};
	struct cloud_msg cloud_msg = {
		.type = CLOUD_PROVISIONING_REQUEST
	};
	struct nrf_provisioning_callback_data event = {
		.type = NRF_PROVISIONING_EVENT_FAILED
	};

	/* Start with a connected network state */
	publish_and_assert(&NETWORK_CHAN, &network_msg);
	wait_for_processing();
	wait_for_cloud_connected(K_SECONDS(WAIT_TIMEOUT));

	/* Trigger provisioning request */
	publish_and_assert(&CLOUD_CHAN, &cloud_msg);
	wait_for_processing();
	wait_for_cloud_disconnected(K_SECONDS(WAIT_TIMEOUT));

	TEST_ASSERT_EQUAL(1, nrf_provisioning_trigger_manually_fake.call_count);

	/* Simulate provisioning failure while network is still connected */
	handler(&event);

	/* Allow time for state machine to process the failure and enter backoff state */
	k_sleep(K_SECONDS(INITIAL_PROVISIONING_RETRY_SEC + 1));

	/* Verify that provisioning is retried after backoff
	 * (indicating we went to backoff state)
	 */
	TEST_ASSERT_EQUAL(2, nrf_provisioning_trigger_manually_fake.call_count);

	/* Exit provisioning by simulating failure */
	handler(&event);
}

void test_provisioning_failed_with_network_disconnected_should_go_to_disconnected(void)
{
	struct network_msg network_msg = {
		.type = NETWORK_CONNECTED
	};
	struct cloud_msg cloud_msg = {
		.type = CLOUD_PROVISIONING_REQUEST
	};
	struct nrf_provisioning_callback_data event = {
		.type = NRF_PROVISIONING_EVENT_FAILED
	};

	/* Start with a connected network state */
	publish_and_assert(&NETWORK_CHAN, &network_msg);
	wait_for_processing();
	wait_for_cloud_connected(K_SECONDS(WAIT_TIMEOUT));

	/* Trigger provisioning request */
	publish_and_assert(&CLOUD_CHAN, &cloud_msg);
	wait_for_processing();
	wait_for_cloud_disconnected(K_SECONDS(WAIT_TIMEOUT));

	TEST_ASSERT_EQUAL(1, nrf_provisioning_trigger_manually_fake.call_count);

	/* Simulate network disconnect while in provisioning state */
	network_msg.type = NETWORK_DISCONNECTED;
	publish_and_assert(&NETWORK_CHAN, &network_msg);
	wait_for_processing();

	/* Simulate provisioning failure while network is disconnected */
	handler(&event);

	/* Allow time for state machine to process the failure and enter disconnected state */
	k_sleep(K_SECONDS(INITIAL_PROVISIONING_RETRY_SEC + 1));

	/* Verify that provisioning is NOT retried (indicating we went to disconnected state) */
	TEST_ASSERT_EQUAL(1, nrf_provisioning_trigger_manually_fake.call_count);
}

/* Test that cloud module correctly handles LOCATION_CLOUD_REQUEST with cellular data */
void test_location_cloud_request_cellular_data(void)
{
	/* Prepare location cloud request with cellular data */
	struct location_msg location_msg = {
		.type = LOCATION_CLOUD_REQUEST,
		.cloud_request = {
			.current_cell = {
				.id = 0x12345678,
				.mcc = 242,
				.mnc = 1,
				.tac = 0x1234,
				.timing_advance = 100,
				.earfcn = 6200,
				.rsrp = -85,
				.rsrq = -12
			},
			.ncells_count = 2,
			.neighbor_cells = {
				{
					.earfcn = 6201,
					.phys_cell_id = 43,
					.rsrp = -90,
					.rsrq = -15,
					.time_diff = 50
				},
				{
					.earfcn = 6202,
					.phys_cell_id = 44,
					.rsrp = -95,
					.rsrq = -18,
					.time_diff = 100
				}
			},
			.gci_cells_count = 0,
			.wifi_cnt = 0
		}
	};

	struct storage_msg storage_data_msg = {
		.type = STORAGE_DATA,
		.data_type = STORAGE_TYPE_LOCATION,
		.data_len = sizeof(location_msg)
	};

	/* Copy location message into storage message buffer */
	memcpy(storage_data_msg.buffer, &location_msg, sizeof(location_msg));

	setup_cloud_and_passthrough();

	/* Send location cloud request via storage data channel */
	publish_and_assert(&STORAGE_DATA_CHAN, &storage_data_msg);
	wait_for_processing();

	/* Verify that nrf_cloud_coap_location_get was called with the cellular data. */
	TEST_ASSERT_EQUAL(1, nrf_cloud_coap_location_get_fake.call_count);
}

/* Test that cloud module correctly handles LOCATION_CLOUD_REQUEST with Wi-Fi data */
void test_location_cloud_request_wifi_data(void)
{

	/* Prepare location cloud request with Wi-Fi data */
	struct location_msg location_msg = {
		.type = LOCATION_CLOUD_REQUEST,
		.cloud_request = {
			.current_cell = {
				.id = LTE_LC_CELL_EUTRAN_ID_INVALID
			},
			.ncells_count = 0,
			.gci_cells_count = 0,
			.wifi_cnt = 3,
			.wifi_aps = {
				{
					.rssi = -50,
					.mac = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x11},
					.mac_length = 6
				},
				{
					.rssi = -55,
					.mac = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x22},
					.mac_length = 6
				},
				{
					.rssi = -60,
					.mac = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x33},
					.mac_length = 6
				}
			}
		}
	};

	struct storage_msg storage_data_msg = {
		.type = STORAGE_DATA,
		.data_type = STORAGE_TYPE_LOCATION,
		.data_len = sizeof(location_msg)
	};

	/* Copy location message into storage message buffer */
	memcpy(storage_data_msg.buffer, &location_msg, sizeof(location_msg));

	setup_cloud_and_passthrough();

	/* Send location cloud request via storage data channel */
	publish_and_assert(&STORAGE_DATA_CHAN, &storage_data_msg);
	wait_for_processing();

	/* Verify that nrf_cloud_coap_location_get was called with the Wi-Fi data. */
	TEST_ASSERT_EQUAL(1, nrf_cloud_coap_location_get_fake.call_count);
}

/* Test that cloud module correctly handles LOCATION_CLOUD_REQUEST
 * with both cellular and Wi-Fi data
 */
void test_location_cloud_request_combined_data(void)
{

	/* Prepare location cloud request with both cellular and Wi-Fi data */
	struct location_msg location_msg = {
		.type = LOCATION_CLOUD_REQUEST,
		.cloud_request = {
			.current_cell = {
				.id = 0x87654321,
				.mcc = 242,
				.mnc = 2,
				.tac = 0x5678,
				.timing_advance = 200,
				.earfcn = 3400,
				.rsrp = -75,
				.rsrq = -9
			},
			.ncells_count = 1,
			.neighbor_cells = {
				{
					.earfcn = 3401,
					.phys_cell_id = 100,
					.rsrp = -80,
					.rsrq = -11,
					.time_diff = 150
				}
			},
			.gci_cells_count = 0,
			.wifi_cnt = 2,
			.wifi_aps = {
				{
					.rssi = -45,
					.mac = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66},
					.mac_length = 6
				},
				{
					.rssi = -48,
					.mac = {0x66, 0x55, 0x44, 0x33, 0x22, 0x11},
					.mac_length = 6
				}
			}
		}
	};

	struct storage_msg storage_data_msg = {
		.type = STORAGE_DATA,
		.data_type = STORAGE_TYPE_LOCATION,
		.data_len = sizeof(location_msg)
	};

	/* Copy location message into storage message buffer */
	memcpy(storage_data_msg.buffer, &location_msg, sizeof(location_msg));

	setup_cloud_and_passthrough();

	/* Send location cloud request via storage data channel */
	publish_and_assert(&STORAGE_DATA_CHAN, &storage_data_msg);
	wait_for_processing();

	/* Verify that nrf_cloud_coap_location_get was called with both data types. */
	TEST_ASSERT_EQUAL(1, nrf_cloud_coap_location_get_fake.call_count);
}

static int nrf_cloud_coap_location_get_custom_fake(
	const struct nrf_cloud_rest_location_request *request,
	struct nrf_cloud_location_result *const result)
{
	ARG_UNUSED(result);

	/* This custom fake allows us to inspect the content of the opaque
	 * nrf_cloud_rest_location_request struct, which is not possible in the
	 * test function itself.
	 */
	TEST_ASSERT_NOT_NULL(request->wifi_info);
	TEST_ASSERT_EQUAL(2, request->wifi_info->cnt);

	return 0;
}

/* Test that cloud module correctly handles LOCATION_CLOUD_REQUEST with Wi-Fi data,
 * specifically when the number of APs is less than the maximum configured count.
 * This verifies the fix for the bug where uninitialized data was being sent.
 */
void test_location_cloud_request_wifi_data_partial_ap_list(void)
{
	/* Prepare location cloud request with 2 Wi-Fi APs, which is less than
	 * CONFIG_LOCATION_METHOD_WIFI_SCANNING_RESULTS_MAX_CNT.
	 */
	struct storage_msg storage_data_msg = {
		.type = STORAGE_DATA,
		.data_type = STORAGE_TYPE_LOCATION,
		.data_len = sizeof(struct location_msg)
	};

	struct location_msg *location_msg = (struct location_msg *)storage_data_msg.buffer;
	location_msg->type = LOCATION_CLOUD_REQUEST;
	location_msg->cloud_request.current_cell.id = LTE_LC_CELL_EUTRAN_ID_INVALID;
	location_msg->cloud_request.ncells_count = 0;
	location_msg->cloud_request.gci_cells_count = 0;
	location_msg->cloud_request.wifi_cnt = 2;
	location_msg->cloud_request.wifi_aps[0].rssi = -65;
	location_msg->cloud_request.wifi_aps[0].mac[0] = 0xDE;
	location_msg->cloud_request.wifi_aps[0].mac[1] = 0xAD;
	location_msg->cloud_request.wifi_aps[0].mac[2] = 0xBE;
	location_msg->cloud_request.wifi_aps[0].mac[3] = 0xEF;
	location_msg->cloud_request.wifi_aps[0].mac[4] = 0x00;
	location_msg->cloud_request.wifi_aps[0].mac[5] = 0x01;
	location_msg->cloud_request.wifi_aps[0].mac_length = 6;
	location_msg->cloud_request.wifi_aps[1].rssi = -70;
	location_msg->cloud_request.wifi_aps[1].mac[0] = 0xDE;
	location_msg->cloud_request.wifi_aps[1].mac[1] = 0xAD;
	location_msg->cloud_request.wifi_aps[1].mac[2] = 0xBE;
	location_msg->cloud_request.wifi_aps[1].mac[3] = 0xEF;
	location_msg->cloud_request.wifi_aps[1].mac[4] = 0x00;
	location_msg->cloud_request.wifi_aps[1].mac[5] = 0x02;
	location_msg->cloud_request.wifi_aps[1].mac_length = 6;

	setup_cloud_and_passthrough();

	/* Set custom fake to verify data within the call */
	nrf_cloud_coap_location_get_fake.custom_fake = nrf_cloud_coap_location_get_custom_fake;

	/* Send location cloud request via storage data channel */
	publish_and_assert(&STORAGE_DATA_CHAN, &storage_data_msg);
	wait_for_processing();

	/* Verify that nrf_cloud_coap_location_get was called. */
	TEST_ASSERT_EQUAL(1, nrf_cloud_coap_location_get_fake.call_count);
}

/* Test that wifi_ap_data_construct validation catches excessive wifi_cnt.
 * This test verifies that when wifi_cnt exceeds the configured maximum
 * (CONFIG_LOCATION_METHOD_WIFI_SCANNING_RESULTS_MAX_CNT), the request
 * is not sent.
 */
void test_location_cloud_request_wifi_data_excessive_ap_count(void)
{
	/* Prepare location cloud request with wifi_cnt that exceeds the maximum */
	struct location_msg location_msg = {
		.type = LOCATION_CLOUD_REQUEST,
		.cloud_request = {
			.current_cell = {
				.id = LTE_LC_CELL_EUTRAN_ID_INVALID
			},
			.ncells_count = 0,
			.gci_cells_count = 0,
			/* Set wifi_cnt to exceed
			 * CONFIG_LOCATION_METHOD_WIFI_SCANNING_RESULTS_MAX_CNT
			 */
			.wifi_cnt = CONFIG_LOCATION_METHOD_WIFI_SCANNING_RESULTS_MAX_CNT + 1,
		}
	};

	/* Initialize at least one AP to avoid other validation errors */
	location_msg.cloud_request.wifi_aps[0].rssi = -50;
	location_msg.cloud_request.wifi_aps[0].mac[0] = 0xAA;
	location_msg.cloud_request.wifi_aps[0].mac[1] = 0xBB;
	location_msg.cloud_request.wifi_aps[0].mac[2] = 0xCC;
	location_msg.cloud_request.wifi_aps[0].mac[3] = 0xDD;
	location_msg.cloud_request.wifi_aps[0].mac[4] = 0xEE;
	location_msg.cloud_request.wifi_aps[0].mac[5] = 0xFF;
	location_msg.cloud_request.wifi_aps[0].mac_length = 6;

	struct storage_msg storage_data_msg = {
		.type = STORAGE_DATA,
		.data_type = STORAGE_TYPE_LOCATION,
		.data_len = sizeof(location_msg)
	};

	/* Copy location message into storage message buffer */
	memcpy(storage_data_msg.buffer, &location_msg, sizeof(location_msg));

	setup_cloud_and_passthrough();

	/* Send location cloud request via storage data channel */
	publish_and_assert(&STORAGE_DATA_CHAN, &storage_data_msg);
	wait_for_processing();

	/* Verify that nrf_cloud_coap_location_get was NOT called due to validation failure. */
	TEST_ASSERT_EQUAL(0, nrf_cloud_coap_location_get_fake.call_count);
}

void test_shadow_get_desired_should_return_empty(void)
{
	struct cloud_msg request_msg = {
		.type = CLOUD_SHADOW_GET_DESIRED
	};

	/* Configure fake to simulate empty desired section */
	nrf_cloud_coap_shadow_get_fake.custom_fake = nrf_cloud_coap_shadow_get_empty_fake;

	/* Connect to cloud */
	test_should_transition_from_disconnected_to_connected_ready();

	/* Send shadow get desired request */
	publish_and_assert(&CLOUD_CHAN, &request_msg);

	/* Consume the request message (echo from our own publish) */
	wait_for_cloud_message(CLOUD_SHADOW_GET_DESIRED, K_MSEC(100));

	/* Give cloud module time to process the request */
	wait_for_processing();

	/* Wait for and verify the response message */
	wait_for_cloud_message(CLOUD_SHADOW_RESPONSE_EMPTY_DESIRED, K_SECONDS(5));

	/* Verify that nrf_cloud_coap_shadow_get was called */
	TEST_ASSERT_EQUAL(1, nrf_cloud_coap_shadow_get_fake.call_count);
}

void test_shadow_get_delta_should_return_empty(void)
{
	struct cloud_msg request_msg = {
		.type = CLOUD_SHADOW_GET_DELTA
	};

	/* Configure fake to simulate empty desired section */
	nrf_cloud_coap_shadow_get_fake.custom_fake = nrf_cloud_coap_shadow_get_empty_fake;

	/* Connect to cloud */
	test_should_transition_from_disconnected_to_connected_ready();

	/* Send shadow get desired request */
	publish_and_assert(&CLOUD_CHAN, &request_msg);

	/* Consume the request message (echo from our own publish) */
	wait_for_cloud_message(CLOUD_SHADOW_GET_DELTA, K_MSEC(100));

	/* Give cloud module time to process the request */
	wait_for_processing();

	/* Wait for and verify the response message */
	wait_for_cloud_message(CLOUD_SHADOW_RESPONSE_EMPTY_DELTA, K_SECONDS(5));

	/* Verify that nrf_cloud_coap_shadow_get was called */
	TEST_ASSERT_EQUAL(1, nrf_cloud_coap_shadow_get_fake.call_count);
}

void test_shadow_get_desired_should_return_response(void)
{
	struct cloud_msg request_msg = {
		.type = CLOUD_SHADOW_GET_DESIRED
	};

	/* Configure fake to simulate empty desired section */
	nrf_cloud_coap_shadow_get_fake.custom_fake = nrf_cloud_coap_shadow_get_response_fake;

	/* Connect to cloud */
	test_should_transition_from_disconnected_to_connected_ready();

	/* Send shadow get desired request */
	publish_and_assert(&CLOUD_CHAN, &request_msg);

	/* Consume the request message (echo from our own publish) */
	wait_for_cloud_message(CLOUD_SHADOW_GET_DESIRED, K_MSEC(100));

	/* Give cloud module time to process the request */
	wait_for_processing();

	/* Wait for and verify the response message */
	wait_for_cloud_message(CLOUD_SHADOW_RESPONSE_DESIRED, K_SECONDS(5));

	/* Verify that nrf_cloud_coap_shadow_get was called */
	TEST_ASSERT_EQUAL(1, nrf_cloud_coap_shadow_get_fake.call_count);
}

void test_shadow_get_delta_should_return_response(void)
{
	struct cloud_msg request_msg = {
		.type = CLOUD_SHADOW_GET_DELTA
	};

	/* Configure fake to simulate empty desired section */
	nrf_cloud_coap_shadow_get_fake.custom_fake = nrf_cloud_coap_shadow_get_response_fake;

	/* Connect to cloud */
	test_should_transition_from_disconnected_to_connected_ready();

	/* Send shadow get desired request */
	publish_and_assert(&CLOUD_CHAN, &request_msg);

	/* Consume the request message (echo from our own publish) */
	wait_for_cloud_message(CLOUD_SHADOW_GET_DELTA, K_MSEC(100));

	/* Give cloud module time to process the request */
	wait_for_processing();

	/* Wait for and verify the response message */
	wait_for_cloud_message(CLOUD_SHADOW_RESPONSE_DELTA, K_SECONDS(5));

	/* Verify that nrf_cloud_coap_shadow_get was called */
	TEST_ASSERT_EQUAL(1, nrf_cloud_coap_shadow_get_fake.call_count);
}

void test_shadow_update_reported_should_call_patch(void)
{
	const uint8_t test_cbor_payload[] = {0xa3, 0x01, 0x02, 0x03, 0x04};
	struct cloud_msg update_msg = {
		.type = CLOUD_SHADOW_UPDATE_REPORTED,
		.payload = {
			.buffer_data_len = sizeof(test_cbor_payload)
		}
	};

	/* Copy test payload into message buffer */
	memcpy(update_msg.payload.buffer, test_cbor_payload, sizeof(test_cbor_payload));

	/* Connect to cloud */
	test_should_transition_from_disconnected_to_connected_ready();

	/* Send shadow update reported request */
	publish_and_assert(&CLOUD_CHAN, &update_msg);

	/* Give cloud module time to process the request */
	wait_for_processing();

	/* Verify that nrf_cloud_coap_patch was called */
	TEST_ASSERT_EQUAL(1, nrf_cloud_coap_patch_fake.call_count);

	/* Verify the patch was called with correct parameters */
	TEST_ASSERT_EQUAL_STRING("state/reported", nrf_cloud_coap_patch_fake.arg0_val);
	TEST_ASSERT_NULL(nrf_cloud_coap_patch_fake.arg1_val); /* app_id is NULL */
	TEST_ASSERT_EQUAL(sizeof(test_cbor_payload), nrf_cloud_coap_patch_fake.arg3_val);
	TEST_ASSERT_EQUAL(COAP_CONTENT_FORMAT_APP_CBOR, nrf_cloud_coap_patch_fake.arg4_val);
	TEST_ASSERT_TRUE(nrf_cloud_coap_patch_fake.arg5_val); /* confirmable = true */

	/* Verify buffer contents match (not pointer, since message is copied) */
	TEST_ASSERT_EQUAL_UINT8_ARRAY(test_cbor_payload, nrf_cloud_coap_patch_fake.arg2_val,
				      sizeof(test_cbor_payload));
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
