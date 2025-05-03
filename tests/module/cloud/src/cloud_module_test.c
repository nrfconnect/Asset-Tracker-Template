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
#include <date_time.h>

#include "environmental.h"
#include "cloud.h"
#include "cloud_codec.h"
#include "power.h"
#include "network.h"
#include "location.h"
#include "app_common.h"
#include "storage.h"
#include "expected_environmental_cbor.h"

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
ZBUS_CHAN_DEFINE(STORAGE_CHAN,
		 struct storage_msg,
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
FAKE_VALUE_FUNC(int, nrf_cloud_coap_location_send, const struct nrf_cloud_gnss_data *, bool);
FAKE_VALUE_FUNC(int64_t, date_time_now, int64_t *);
FAKE_VOID_FUNC(location_cloud_location_ext_result_set, enum location_ext_result,
	       struct location_data *);
FAKE_VALUE_FUNC(int, location_agnss_data_process, const char *, size_t);
FAKE_VALUE_FUNC(int, nrf_cloud_coap_post, const char *, const char *,
		const uint8_t *, size_t,
		enum coap_content_format, bool,
		coap_client_response_cb_t, void *);
FAKE_VALUE_FUNC(int, date_time_now, int64_t *);

/* Forward declarations */
static void dummy_cb(const struct zbus_channel *chan);
static void listener_cb(const struct zbus_channel *chan);

/* Define unused subscribers */
ZBUS_SUBSCRIBER_DEFINE(app, 1);
ZBUS_SUBSCRIBER_DEFINE(battery, 1);
ZBUS_SUBSCRIBER_DEFINE(environmental, 1);
ZBUS_SUBSCRIBER_DEFINE(fota, 1);
ZBUS_SUBSCRIBER_DEFINE(led, 1);
ZBUS_SUBSCRIBER_DEFINE(location, 1);
ZBUS_LISTENER_DEFINE(trigger, dummy_cb);
ZBUS_LISTENER_DEFINE(cloud_test_listener, listener_cb);
ZBUS_LISTENER_DEFINE(storage_test_listener, listener_cb);

#define FAKE_DEVICE_ID		"test_device"

static K_SEM_DEFINE(cloud_disconnected, 0, 1);
static K_SEM_DEFINE(cloud_connected, 0, 1);
static K_SEM_DEFINE(data_sent, 0, 1);

static struct storage_msg recv_storage_msg;
/* Used to determine which of the CBOR buffers in expected_environmental_cbor.c to use */
static const uint8_t *expected_cbor_data_ptr;

static int nrf_cloud_client_id_get_custom_fake(char *buf, size_t len)
{
	TEST_ASSERT(len >= sizeof(FAKE_DEVICE_ID));
	memcpy(buf, FAKE_DEVICE_ID, sizeof(FAKE_DEVICE_ID));

	return 0;
}

static int nrf_cloud_coap_post_custom_fake(const char *resource, const char *query,
		const uint8_t *buf, size_t len,
		enum coap_content_format fmt, bool confirmable,
		coap_client_response_cb_t cb, void *user)
{
	ARG_UNUSED(resource);
	ARG_UNUSED(query);
	ARG_UNUSED(fmt);
	ARG_UNUSED(confirmable);
	ARG_UNUSED(cb);
	ARG_UNUSED(user);

	TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_cbor_data_ptr, buf, len);

	return 0;
}

/* Custom fake for date_time_now that sets a fixed timestamp */
static int date_time_now_custom_fake(int64_t *time_ms)
{
	*time_ms = 1621500000000; /* Fixed timestamp for May 20, 2025 */

	return 0;
}

static void dummy_cb(const struct zbus_channel *chan)
{
	ARG_UNUSED(chan);
}

static void listener_cb(const struct zbus_channel *chan)
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

	if (chan == &STORAGE_CHAN) {
		const struct storage_msg *storage_msg = zbus_chan_const_msg(chan);

		recv_storage_msg = *storage_msg;
	}
}

static void free_fifo_chunk(struct storage_data_chunk *chunk)
{
	chunk->data.buf = NULL;
}

void setUp(void)
{
	const struct zbus_channel *chan;

	/* Reset fakes */
	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(task_wdt_add);
	RESET_FAKE(nrf_cloud_client_id_get);
	RESET_FAKE(nrf_cloud_coap_json_message_send);
	RESET_FAKE(nrf_cloud_coap_connect);
	RESET_FAKE(nrf_cloud_coap_location_send);
	RESET_FAKE(date_time_now);
	RESET_FAKE(nrf_cloud_coap_post);

	nrf_cloud_client_id_get_fake.custom_fake = nrf_cloud_client_id_get_custom_fake;
	date_time_now_fake.custom_fake = date_time_now_custom_fake;
	nrf_cloud_coap_post_fake.custom_fake = nrf_cloud_coap_post_custom_fake;

	/* Set default return values */
	nrf_cloud_coap_location_send_fake.return_val = 0;
	date_time_now_fake.return_val = 1640995200000; /* 2022-01-01 00:00:00 UTC in ms */

	/* Clear all channels */
	zbus_sub_wait(&location, &chan, K_NO_WAIT);
	zbus_sub_wait(&app, &chan, K_NO_WAIT);
	zbus_sub_wait(&fota, &chan, K_NO_WAIT);
	zbus_sub_wait(&led, &chan, K_NO_WAIT);
	zbus_sub_wait(&battery, &chan, K_NO_WAIT);

	zbus_chan_add_obs(&CLOUD_CHAN, &cloud_test_listener, K_NO_WAIT);
	zbus_chan_add_obs(&STORAGE_CHAN, &cloud_test_listener, K_NO_WAIT);

	/* Reset received message */
	memset(&recv_storage_msg, 0, sizeof(recv_storage_msg));
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
	struct network_msg msg = {
		.type = NETWORK_CONNECTED,
	};

	zbus_chan_pub(&NETWORK_CHAN, &msg, K_NO_WAIT);

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
	struct network_msg msg = {
		.type = NETWORK_DISCONNECTED,
	};

	zbus_chan_pub(&NETWORK_CHAN, &msg, K_NO_WAIT);

	/* Transport module needs CPU to run state machine */
	k_sleep(K_MSEC(100));

	err = k_sem_take(&cloud_disconnected, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
}

void test_connected_disconnected_to_connected_send_payload_disconnect(void)
{
	int err;
	struct network_msg network_msg = {
		.type = NETWORK_CONNECTED,
	};
	struct cloud_msg msg = {
		.type = CLOUD_PAYLOAD_JSON,
		.payload.buffer = "{\"Another\": \"1\"}",
		.payload.buffer_data_len = strnlen(msg.payload.buffer, sizeof(msg.payload.buffer)),
	};

	/* Reset call count */
	nrf_cloud_coap_bytes_send_fake.call_count = 0;

	err = zbus_chan_pub(&NETWORK_CHAN, &network_msg, K_NO_WAIT);
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

	status = NETWORK_DISCONNECTED;

	err = zbus_chan_pub(&NETWORK_CHAN, &status, K_NO_WAIT);
	TEST_ASSERT_EQUAL(0, err);

	/* Transport module needs CPU to run state machine */
	k_sleep(K_MSEC(100));

	err = k_sem_take(&cloud_disconnected, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
}

/* Test GNSS location data handling */
void test_gnss_location_data_handling(void)
{
	int err;
	enum network_msg_type status = NETWORK_CONNECTED;
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

	/* Connect to cloud */
	zbus_chan_pub(&NETWORK_CHAN, &status, K_NO_WAIT);

	err = k_sem_take(&cloud_connected, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Send GNSS location data */
	err = zbus_chan_pub(&LOCATION_CHAN, &location_msg, K_NO_WAIT);
	TEST_ASSERT_EQUAL(0, err);

	/* Give the module time to process */
	k_sleep(K_MSEC(100));

	/* Verify that GNSS location data was sent to nRF Cloud */
	TEST_ASSERT_EQUAL(1, nrf_cloud_coap_location_send_fake.call_count);

	/* Basic verification that the function was called with valid arguments */
	if (nrf_cloud_coap_location_send_fake.call_count > 0) {
		TEST_ASSERT_NOT_NULL(nrf_cloud_coap_location_send_fake.arg0_val);
	}
}

void test_codec_encode_environmental_data_single(void)
{
	int err;
	uint8_t payload[128];
	size_t payload_len = sizeof(payload);
	size_t payload_out_len;

	err = encode_environmental_sample(payload, payload_len, &payload_out_len, env_samples, 0);

	TEST_ASSERT_EQUAL(0, err);
	TEST_ASSERT_EQUAL(payload_out_len, expected_environmental_single_cbor_len);

	/* Test single sample encoding with null parameters */
	err = encode_environmental_sample(NULL, payload_len, &payload_out_len, env_samples, 0);
	TEST_ASSERT_EQUAL(-EINVAL, err);

	err = encode_environmental_sample(payload, payload_len, NULL, env_samples, 0);
	TEST_ASSERT_EQUAL(-EINVAL, err);

	err = encode_environmental_sample(payload, payload_len, &payload_out_len, NULL, 0);
	TEST_ASSERT_EQUAL(-EINVAL, err);
}

/* Test the environmental data encoding functions */
void test_codec_encode_environmental_data_array(void)
{
	int err;
	/* Create test environmental data */
	uint8_t payload[4096];
	size_t payload_len = sizeof(payload);
	size_t payload_out_len;

	/* Test encoding array of samples */
	err = encode_environmental_data_array(
		payload, payload_len, &payload_out_len, env_samples, ARRAY_SIZE(env_samples));

	TEST_ASSERT_EQUAL(0, err);
	TEST_ASSERT_EQUAL(payload_out_len, expected_environmental_cbor_33_len);

	/* Check that the output matches the expected CBOR */
	TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_environmental_cbor_33, payload, payload_out_len);

	/* Test encoding with null parameters */
	err = encode_environmental_data_array(NULL, payload_len, &payload_out_len, env_samples, 2);
	TEST_ASSERT_EQUAL(-EINVAL, err);

	err = encode_environmental_data_array(payload, payload_len, NULL, env_samples, 2);
	TEST_ASSERT_EQUAL(-EINVAL, err);

	err = encode_environmental_data_array(payload, payload_len, &payload_out_len, NULL, 2);
	TEST_ASSERT_EQUAL(-EINVAL, err);

	err = encode_environmental_data_array(
		payload, payload_len, &payload_out_len, env_samples, 0);
	TEST_ASSERT_EQUAL(-EINVAL, err);

	/* Test encoding with buffer too small */
	err = encode_environmental_data_array(payload, 10, &payload_out_len, env_samples, 2);
	TEST_ASSERT_EQUAL(-EIO, err);
}

void test_codec_encode_data_chunk_array(void)
{
	int err;
	/* Create test environmental data */
	uint8_t payload[4096];
	size_t payload_len = sizeof(payload);
	size_t payload_out_len;
	struct storage_data_chunk chunk_data[20] = {0};
	struct storage_data_chunk *chunks[20];

	for (size_t i = 0; i < ARRAY_SIZE(chunks); i++) {
		chunk_data[i].type = STORAGE_TYPE_ENVIRONMENTAL;
		chunk_data[i].data.ENVIRONMENTAL = env_samples[i];
		chunks[i] = &chunk_data[i];
	};

	/* Test encoding array of samples */
	err = encode_data_chunk_array(
		payload, payload_len, &payload_out_len, chunks, ARRAY_SIZE(chunks));

	TEST_ASSERT_EQUAL(0, err);
	TEST_ASSERT_EQUAL(payload_out_len, expected_environmental_cbor_20_len);

	/* Check that the output matches the expected CBOR */
	TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_environmental_cbor_20, payload, payload_out_len);
}

void test_send_storage_fifo_request(void)
{
	int err;
	struct storage_msg msg = {
		.type = STORAGE_FIFO_REQUEST,
	};

	err = zbus_chan_pub(&STORAGE_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	TEST_ASSERT_EQUAL(STORAGE_FIFO_REQUEST, recv_storage_msg.type);
}

void test_receive_storage_fifo_available_20_chunks(void)
{
	int err;
	struct k_fifo storage_fifo;
	struct storage_msg msg = {
		.type = STORAGE_FIFO_AVAILABLE,
		.fifo = &storage_fifo,
		.data_len = 0,
	};
	struct storage_data_chunk chunks[20] = {0};
	/* With a 1024-byte buffer and 76 bytes per environmental chunk + 2 byte header,
	 * we can fit approximately 13 chunks: (1024 - 2) / 76 = 13.44
	 * The remaining chunks should still be in the FIFO.
	 */
	size_t expected_processed_chunks =
		MIN(ARRAY_SIZE(chunks), (CONFIG_APP_CLOUD_PAYLOAD_BUFFER_MAX_SIZE - 2) /
		 expected_environmental_single_cbor_len);

	expected_cbor_data_ptr = expected_environmental_cbor_13;

	/* Initialize the FIFO */
	k_fifo_init(&storage_fifo);

	/* Populate environmental samples and storage chunks, then put them in the FIFO.
	 * This simulates the data that would be stored in the FIFO.
	 * Each chunk corresponds to an environmental sample.
	 */
	for (size_t i = 0; i < ARRAY_SIZE(chunks); i++) {
		chunks[i].type = STORAGE_TYPE_ENVIRONMENTAL;
		chunks[i].data.ENVIRONMENTAL = env_samples[i];
		chunks[i].finished = free_fifo_chunk;

		k_fifo_put(&storage_fifo, &chunks[i]);

		msg.data_len++;
	};

	err = zbus_chan_pub(&STORAGE_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	TEST_ASSERT_EQUAL(STORAGE_FIFO_AVAILABLE, recv_storage_msg.type);
	TEST_ASSERT_EQUAL(msg.data_len, recv_storage_msg.data_len);

	k_sleep(K_SECONDS(1));

	for (size_t i = 0; i < expected_processed_chunks; i++) {
		TEST_ASSERT_EQUAL(NULL, chunks[i].data.buf);
	}
}

void test_receive_storage_fifo_available_5_chunks(void)
{
	int err;
	struct k_fifo storage_fifo;
	struct storage_msg msg = {
		.type = STORAGE_FIFO_AVAILABLE,
		.fifo = &storage_fifo,
		.data_len = 0,
	};
	struct storage_data_chunk chunks[5] = {0};
	/* With a 1024-byte buffer and 76 bytes per environmental chunk + 2 byte header,
	 * we can fit approximately 13 chunks: (1024 - 2) / 76 = 13.44
	 * The remaining chunks should still be in the FIFO.
	 */
	size_t expected_processed_chunks =
		MIN(ARRAY_SIZE(chunks), (CONFIG_APP_CLOUD_PAYLOAD_BUFFER_MAX_SIZE - 2) /
		 expected_environmental_single_cbor_len);

	/* Initialize the FIFO */
	k_fifo_init(&storage_fifo);

	expected_cbor_data_ptr = expected_environmental_cbor_5;

	/* Populate environmental samples and storage chunks, then put them in the FIFO.
	 * This simulates the data that would be stored in the FIFO.
	 * Each chunk corresponds to an environmental sample.
	 */
	for (size_t i = 0; i < ARRAY_SIZE(chunks); i++) {
		chunks[i].type = STORAGE_TYPE_ENVIRONMENTAL;
		chunks[i].data.ENVIRONMENTAL = env_samples[i];
		chunks[i].finished = free_fifo_chunk;

		k_fifo_put(&storage_fifo, &chunks[i]);

		msg.data_len++;
	};

	err = zbus_chan_pub(&STORAGE_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	TEST_ASSERT_EQUAL(STORAGE_FIFO_AVAILABLE, recv_storage_msg.type);
	TEST_ASSERT_EQUAL(msg.data_len, recv_storage_msg.data_len);

	k_sleep(K_SECONDS(1));

	for (size_t i = 0; i < expected_processed_chunks; i++) {
		TEST_ASSERT_EQUAL(NULL, chunks[i].data.buf);
	}
}

void test_receive_storage_fifo_available_5_chunks_repeatedly(void)
{
	int err;
	struct k_fifo storage_fifo;
	struct storage_msg msg = {
		.type = STORAGE_FIFO_AVAILABLE,
		.fifo = &storage_fifo,
		.data_len = 0,
	};
	struct storage_data_chunk chunks[5] = {0};
	/* With a 1024-byte buffer and 76 bytes per environmental chunk + 2 byte header,
	 * we can fit approximately 13 chunks: (1024 - 2) / 76 = 13.44
	 * The remaining chunks should still be in the FIFO.
	 */
	size_t expected_processed_chunks =
		MIN(ARRAY_SIZE(chunks), (CONFIG_APP_CLOUD_PAYLOAD_BUFFER_MAX_SIZE - 2) /
		 expected_environmental_single_cbor_len);

	/* Initialize the FIFO */
	k_fifo_init(&storage_fifo);

	expected_cbor_data_ptr = expected_environmental_cbor_5;

	for (size_t j = 0; j < 3; j++) {
		msg.data_len = 0;

		/* Populate environmental samples and storage chunks, then put them in the FIFO.
		* This simulates the data that would be stored in the FIFO.
		* Each chunk corresponds to an environmental sample.
		*/
		for (size_t i = 0; i < ARRAY_SIZE(chunks); i++) {
			chunks[i].type = STORAGE_TYPE_ENVIRONMENTAL;
			chunks[i].data.ENVIRONMENTAL = env_samples[i];
			chunks[i].finished = free_fifo_chunk;

			k_fifo_put(&storage_fifo, &chunks[i]);

			msg.data_len++;
		};

		err = zbus_chan_pub(&STORAGE_CHAN, &msg, K_SECONDS(1));
		TEST_ASSERT_EQUAL(0, err);

		TEST_ASSERT_EQUAL(STORAGE_FIFO_AVAILABLE, recv_storage_msg.type);
		TEST_ASSERT_EQUAL(msg.data_len, recv_storage_msg.data_len);

		k_sleep(K_SECONDS(1));

		for (size_t i = 0; i < expected_processed_chunks; i++) {
			TEST_ASSERT_EQUAL(NULL, chunks[i].data.buf);
		}
	}
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
