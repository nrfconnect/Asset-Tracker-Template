/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/* Ensure 'strnlen' is available even with -std=c99. */
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <unity.h>
#include <zephyr/fff.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/smf.h>
#include <zephyr/sys/ring_buffer.h>

#include "storage.h"
#include "storage_backend.h"
#include "storage_data_types.h"
#include "power.h"
#include "environmental.h"
#include "location.h"
#include "app_common.h"
#include "test_samples.h"

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);

/* Define the channels for testing */
ZBUS_CHAN_DEFINE(POWER_CHAN,
		 struct power_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
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

/* Forward declarations */
static void dummy_cb(const struct zbus_channel *chan);
static void storage_chan_cb(const struct zbus_channel *chan);

/* Define unused subscribers */
ZBUS_LISTENER_DEFINE(trigger, dummy_cb);
ZBUS_LISTENER_DEFINE(storage_test_listener, storage_chan_cb);
ZBUS_LISTENER_DEFINE(power_test_listener, dummy_cb);
ZBUS_LISTENER_DEFINE(environmental_test_listener, dummy_cb);
ZBUS_LISTENER_DEFINE(location_test_listener, dummy_cb);

ZBUS_CHAN_ADD_OBS(STORAGE_CHAN, storage_test_listener, 0);
ZBUS_CHAN_ADD_OBS(STORAGE_DATA_CHAN, storage_test_listener, 0);
ZBUS_CHAN_ADD_OBS(POWER_CHAN, power_test_listener, 0);
ZBUS_CHAN_ADD_OBS(ENVIRONMENTAL_CHAN, environmental_test_listener, 0);
ZBUS_CHAN_ADD_OBS(LOCATION_CHAN, location_test_listener, 0);

static double received_battery_samples[ARRAY_SIZE(battery_samples)];
static uint8_t received_battery_samples_count;

static struct environmental_msg received_env_samples[ARRAY_SIZE(env_samples)];
static uint8_t received_env_samples_count;

static struct location_msg received_location_samples[ARRAY_SIZE(location_samples)];
static uint8_t received_location_samples_count;

/* Variables to store received data */
static struct storage_msg received_msg;

static void dummy_cb(const struct zbus_channel *chan)
{
	ARG_UNUSED(chan);
}

static void publish_and_assert(const struct zbus_channel *chan, const void *msg)
{
	int err;

	err = zbus_chan_pub(chan, msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
}

static void populate_env_message(size_t i, struct environmental_msg *env_msg)
{
	env_msg->temperature = env_samples[i].temperature;
	env_msg->humidity = env_samples[i].humidity;
	env_msg->pressure = env_samples[i].pressure;
}

static void populate_all_messages(size_t i, struct power_msg *bat_msg,
				   struct environmental_msg *env_msg,
				   struct location_msg *loc_msg)
{
	bat_msg->percentage = battery_samples[i];
	populate_env_message(i, env_msg);
	*loc_msg = location_samples[i];
}

static void request_batch_and_assert(void)
{
	int err;
	struct storage_msg request_msg = {
		.type = STORAGE_BATCH_REQUEST,
		.session_id = 0x11111111,
	};

	err = zbus_chan_pub(&STORAGE_CHAN, &request_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	TEST_ASSERT_EQUAL(STORAGE_BATCH_REQUEST, received_msg.type);
}

static void close_batch_and_assert(uint32_t session_id)
{
	int err;
	struct storage_msg close_msg = { .type = STORAGE_BATCH_CLOSE, .session_id = session_id };

	err = zbus_chan_pub(&STORAGE_CHAN, &close_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	TEST_ASSERT_EQUAL(STORAGE_BATCH_CLOSE, received_msg.type);
}

static void storage_chan_cb(const struct zbus_channel *chan)
{
	const struct storage_msg *msg = zbus_chan_const_msg(chan);

	if ((chan != &STORAGE_CHAN) && (chan != &STORAGE_DATA_CHAN)) {
		return;
	}

	received_msg = *msg;

	if (msg->type == STORAGE_DATA) {
		switch (msg->data_type) {
		case STORAGE_TYPE_BATTERY:
			received_battery_samples[received_battery_samples_count++] =
				*(const double *)msg->buffer;
			break;
		case STORAGE_TYPE_ENVIRONMENTAL:
			received_env_samples[received_env_samples_count++] =
				*(const struct environmental_msg *)msg->buffer;
			break;
		case STORAGE_TYPE_LOCATION:
			received_location_samples[received_location_samples_count++] =
				*(const struct location_msg *)msg->buffer;
			break;
		default:
			break;
		}

		k_sleep(K_MSEC(10));
	}
}

static size_t read_batch_data(size_t expected_item_count)
{
	struct storage_data_item tmp;
	size_t items_read = 0;

	while (items_read < expected_item_count) {
		int ret = storage_batch_read(&tmp, K_SECONDS(5));

		if (ret == -EAGAIN) {
			/* Timeout - no more data available in this window */
			break;
		}

		TEST_ASSERT_EQUAL(0, ret);

		items_read++;

		switch (tmp.type) {
		case STORAGE_TYPE_BATTERY:
			if (received_battery_samples_count < ARRAY_SIZE(received_battery_samples)) {
				received_battery_samples[received_battery_samples_count++] =
					tmp.data.BATTERY;
			}
			break;
		case STORAGE_TYPE_ENVIRONMENTAL:
			if (received_env_samples_count < ARRAY_SIZE(received_env_samples)) {
				received_env_samples[received_env_samples_count++] =
					tmp.data.ENVIRONMENTAL;
			}
			break;
		case STORAGE_TYPE_LOCATION:
			if (received_location_samples_count <
			    ARRAY_SIZE(received_location_samples)) {
				received_location_samples[received_location_samples_count++] =
					tmp.data.LOCATION;
			}
			break;
		default:
			break;
		}
	}

	return items_read;
}

void setUp(void)
{
	/* Reset fakes */
	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(task_wdt_add);

	received_battery_samples_count = 0;
	received_env_samples_count = 0;
	received_location_samples_count = 0;

	memset(&received_battery_samples, 0, sizeof(received_battery_samples));
	memset(&received_env_samples, 0, sizeof(received_env_samples));
	memset(&received_location_samples, 0, sizeof(received_location_samples));
	memset(&received_msg, 0, sizeof(received_msg));
}

void test_store_retrieve_battery(void)
{
	int err;
	struct power_msg msg = {
		.type = POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE,
	};
	struct storage_msg flush_msg = {
		.type = STORAGE_FLUSH
	};

	for (size_t i = 0; i < ARRAY_SIZE(battery_samples); i++) {
		msg.percentage = battery_samples[i];

		/* Store battery data */
		err = zbus_chan_pub(&POWER_CHAN, &msg, K_SECONDS(1));
		TEST_ASSERT_EQUAL(0, err);
	}

	err = zbus_chan_pub(&STORAGE_CHAN, &flush_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_SECONDS(10));

	for (size_t i = 0; i < CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE; i++) {
		const size_t sample_idx =
			(ARRAY_SIZE(battery_samples) - CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE) + i;

		/* Verify received data */
		TEST_ASSERT_EQUAL_DOUBLE(battery_samples[sample_idx], received_battery_samples[i]);
	}
}

void test_store_retrieve_environmental(void)
{
	int err;
	struct environmental_msg env_msg = {
		.type = ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE,
	};
	struct storage_msg flush_msg = {
		.type = STORAGE_FLUSH
	};

	for (size_t i = 0; i < ARRAY_SIZE(env_samples); i++) {
		populate_env_message(i, &env_msg);

		/* Store environmental data */
		publish_and_assert(&ENVIRONMENTAL_CHAN, &env_msg);
	}

	err = zbus_chan_pub(&STORAGE_CHAN, &flush_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_SECONDS(10));

	for (size_t i = 0; i < CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE; i++) {
		const size_t sample_idx =
			(ARRAY_SIZE(env_samples) - CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE) + i;

		TEST_ASSERT_EQUAL_DOUBLE(env_samples[sample_idx].temperature,
					 received_env_samples[i].temperature);
		TEST_ASSERT_EQUAL_DOUBLE(env_samples[sample_idx].humidity,
					 received_env_samples[i].humidity);
		TEST_ASSERT_EQUAL_DOUBLE(env_samples[sample_idx].pressure,
					 received_env_samples[i].pressure);
	}
}

void test_receive_mixed_data(void)
{
	int err;
	struct power_msg bat_msg = {
		.type = POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE,
	};
	struct environmental_msg env_msg = {
		.type = ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE,
	};
	struct location_msg loc_msg = {
		.type = LOCATION_GNSS_DATA,
	};
	struct storage_msg flush_msg = {
		.type = STORAGE_FLUSH
	};
	const uint8_t num_samples = CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE;

	for (size_t i = 0; i < num_samples; i++) {
		populate_all_messages(i, &bat_msg, &env_msg, &loc_msg);

		/* Store battery data */
		publish_and_assert(&POWER_CHAN, &bat_msg);

		/* Store environmental data */
		publish_and_assert(&ENVIRONMENTAL_CHAN, &env_msg);

		/* Store location data */
		publish_and_assert(&LOCATION_CHAN, &loc_msg);
	}

	err = zbus_chan_pub(&STORAGE_CHAN, &flush_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_SECONDS(10));

	TEST_ASSERT_EQUAL(received_battery_samples_count, MIN(num_samples,
				CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE));
	TEST_ASSERT_EQUAL(received_env_samples_count, MIN(num_samples,
				CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE));
	TEST_ASSERT_EQUAL(received_location_samples_count, MIN(num_samples,
				CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE));

	for (size_t i = 0; i < num_samples; i++) {
		/* Since we only store the first num_samples from each array, and storage
		 * capacity is sufficient to hold all of them, we compare directly with
		 * the indices that were actually stored (0 to num_samples-1)
		 */

		/* Only perform assertions for samples that were actually received */
		if (i < received_battery_samples_count) {
			TEST_ASSERT_EQUAL_DOUBLE(battery_samples[i],
						 received_battery_samples[i]);
		}

		if (i < received_env_samples_count) {
			TEST_ASSERT_EQUAL_DOUBLE(env_samples[i].temperature,
						 received_env_samples[i].temperature);
			TEST_ASSERT_EQUAL_DOUBLE(env_samples[i].humidity,
						 received_env_samples[i].humidity);
			TEST_ASSERT_EQUAL_DOUBLE(env_samples[i].pressure,
						 received_env_samples[i].pressure);
		}

		if (i < received_location_samples_count) {
			TEST_ASSERT_EQUAL_DOUBLE(location_samples[i].gnss_data.latitude,
					received_location_samples[i].gnss_data.latitude);
			TEST_ASSERT_EQUAL_DOUBLE(location_samples[i].gnss_data.longitude,
					received_location_samples[i].gnss_data.longitude);
			TEST_ASSERT_EQUAL_FLOAT(location_samples[i].gnss_data.accuracy,
					received_location_samples[i].gnss_data.accuracy);
		}
	}
}

void test_storage_batch_request_empty(void)
{
	int err;
	struct storage_msg msg = {
		.type = STORAGE_BATCH_REQUEST,
		.session_id = 0x22222222,
	};

	/* Request batch data */
	err = zbus_chan_pub(&STORAGE_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	TEST_ASSERT_EQUAL(STORAGE_BATCH_REQUEST, received_msg.type);

	/* Wait for the batch response */
	k_sleep(K_SECONDS(1));

	/* Check if the batch is empty */
	TEST_ASSERT_EQUAL(STORAGE_BATCH_EMPTY, received_msg.type);
	TEST_ASSERT_EQUAL(0, received_msg.data_len);

	/* Close the batch session as required by API */
	close_batch_and_assert(received_msg.session_id);
}

void test_storage_batch_request_and_retrieve(void)
{
	int err;
	struct environmental_msg env_msg = { .type = ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE };
	struct storage_msg clear_msg = { .type = STORAGE_CLEAR };
	const uint8_t num_samples = 30;
	size_t items_read;

	for (size_t i = 0; i < num_samples; i++) {
		populate_env_message(i, &env_msg);

		/* Store environmental data */
		publish_and_assert(&ENVIRONMENTAL_CHAN, &env_msg);
	}

	/* Request batch data */
	request_batch_and_assert();

	/* Wait for the batch to be populated */
	k_sleep(K_SECONDS(1));

	TEST_ASSERT_EQUAL(STORAGE_BATCH_AVAILABLE, received_msg.type);
	/* Verify we have data available, but don't assume exact count due to buffer limits */
	TEST_ASSERT_GREATER_THAN(0, received_msg.data_len);

	items_read = read_batch_data(received_msg.data_len);

	for (size_t i = 0; i < items_read; i++) {
		TEST_ASSERT_EQUAL_DOUBLE(env_samples[i].temperature,
					 received_env_samples[i].temperature);
		TEST_ASSERT_EQUAL_DOUBLE(env_samples[i].humidity, received_env_samples[i].humidity);
		TEST_ASSERT_EQUAL_DOUBLE(env_samples[i].pressure, received_env_samples[i].pressure);
	}

	close_batch_and_assert(received_msg.session_id);

	/* Clean up after test */
	err = zbus_chan_pub(&STORAGE_CHAN, &clear_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Allow for storage clear to complete */
	k_sleep(K_SECONDS(1));
}

/* The storage backend can hold a total of CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE items per storage
 * data type. The batch buffer can hold a maximum of CONFIG_APP_STORAGE_BATCH_BUFFER_SIZE bytes.
 * This test will request the batch multiple times to ensure that it can handle multiple requests
 * and that it returns the correct number of items each time, and the correct data, until the
 * storage is empty.
 */
void test_storage_batch_request_multiple(void)
{
	int err;
	struct environmental_msg env_msg = { .type = ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE };
	struct storage_msg request_msg = {
		.type = STORAGE_BATCH_REQUEST,
		.session_id = 0x12121212,
	};
	struct storage_msg clear_msg = { .type = STORAGE_CLEAR };
	const uint8_t num_samples = CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE;
	size_t total_samples_received = 0;

	/* Clear storage at the beginning to ensure clean state */
	err = zbus_chan_pub(&STORAGE_CHAN, &clear_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
	k_sleep(K_SECONDS(1));

	for (size_t i = 0; i < num_samples; i++) {
		populate_env_message(i, &env_msg);

		/* Store environmental data */
		publish_and_assert(&ENVIRONMENTAL_CHAN, &env_msg);
	}

	while (total_samples_received < num_samples) {
		size_t samples_received = 0;

		/* Reset counters before each batch read operation */
		received_env_samples_count = 0;
		memset(&received_env_samples, 0, sizeof(received_env_samples));

		/* Request batch data */
		err = zbus_chan_pub(&STORAGE_CHAN, &request_msg, K_SECONDS(1));
		TEST_ASSERT_EQUAL(0, err);

		TEST_ASSERT_EQUAL(STORAGE_BATCH_REQUEST, received_msg.type);

		/* Wait for the batch to be populated */
		k_sleep(K_SECONDS(1));

		TEST_ASSERT_EQUAL(STORAGE_BATCH_AVAILABLE, received_msg.type);

		/* The data_len reflects items available in batch buffer for this request */
		TEST_ASSERT_GREATER_THAN(0, received_msg.data_len);

		samples_received = read_batch_data(received_msg.data_len);

		/* Compare received samples iteratively - storage consumes data
		 * destructively, so we expect samples in sequence: first iteration
		 * gets 0-15, second gets 16-31, etc.
		 */
		for (size_t i = 0; i < samples_received; i++) {
			size_t expected_sample_idx = total_samples_received + i;

			TEST_ASSERT_EQUAL_DOUBLE(env_samples[expected_sample_idx].temperature,
						received_env_samples[i].temperature);
			TEST_ASSERT_EQUAL_DOUBLE(env_samples[expected_sample_idx].humidity,
						received_env_samples[i].humidity);
			TEST_ASSERT_EQUAL_DOUBLE(env_samples[expected_sample_idx].pressure,
						received_env_samples[i].pressure);
		}

		total_samples_received += samples_received;
	}

	/* Verify we received all expected samples */
	TEST_ASSERT_EQUAL(num_samples, total_samples_received);

	/* Second request should return empty since all data was consumed */
	err = zbus_chan_pub(&STORAGE_CHAN, &request_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	TEST_ASSERT_EQUAL(STORAGE_BATCH_REQUEST, received_msg.type);

	/* Wait for response */
	k_sleep(K_SECONDS(1));

	TEST_ASSERT_EQUAL(STORAGE_BATCH_EMPTY, received_msg.type);

	close_batch_and_assert(received_msg.session_id);

	/* Clean up after test */
	err = zbus_chan_pub(&STORAGE_CHAN, &clear_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Allow for storage clear to complete */
	k_sleep(K_SECONDS(1));
}

void test_storage_batch_clear_when_empty(void)
{
	int err;
	struct storage_msg request = { .type = STORAGE_BATCH_REQUEST, .session_id = 0x33333333 };

	/* Request batch data when storage is empty */
	err = zbus_chan_pub(&STORAGE_CHAN, &request, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Allow handling */
	k_sleep(K_SECONDS(1));

	TEST_ASSERT_EQUAL(STORAGE_BATCH_EMPTY, received_msg.type);

	/* Close the batch session as required by API */
	close_batch_and_assert(received_msg.session_id);
}

void test_storage_batch_request_mixed_data(void)
{
	int err;
	struct power_msg bat_msg = { .type = POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE };
	struct environmental_msg env_msg = { .type = ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE };
	struct location_msg loc_msg = { .type = LOCATION_GNSS_DATA };
	struct storage_msg batch_msg = { .type = STORAGE_BATCH_REQUEST, .session_id = 0x12345678 };
	struct storage_msg clear_msg = { .type = STORAGE_CLEAR };
	const uint8_t num_samples = 30;
	const uint8_t data_types_count = 3;
	uint8_t total_samples_expected = num_samples * data_types_count;
	uint8_t total_samples_received = 0;
	uint8_t total_battery_received = 0;
	uint8_t total_env_received = 0;
	uint8_t total_location_received = 0;

	/* Clear storage at the beginning to ensure clean state */
	err = zbus_chan_pub(&STORAGE_CHAN, &clear_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
	k_sleep(K_SECONDS(1));

	for (size_t i = 0; i < num_samples; i++) {
		populate_all_messages(i, &bat_msg, &env_msg, &loc_msg);

		/* Store battery data */
		publish_and_assert(&POWER_CHAN, &bat_msg);

		/* Store environmental data */
		publish_and_assert(&ENVIRONMENTAL_CHAN, &env_msg);

		/* Store location data */
		publish_and_assert(&LOCATION_CHAN, &loc_msg);
	}

	k_sleep(K_SECONDS(10));

	do {
		size_t items_read;

		/* Reset counters before each batch read operation */
		received_battery_samples_count = 0;
		received_env_samples_count = 0;
		received_location_samples_count = 0;
		memset(&received_battery_samples, 0, sizeof(received_battery_samples));
		memset(&received_env_samples, 0, sizeof(received_env_samples));
		memset(&received_location_samples, 0, sizeof(received_location_samples));

		err = zbus_chan_pub(&STORAGE_CHAN, &batch_msg, K_SECONDS(1));
		TEST_ASSERT_EQUAL(0, err);

		TEST_ASSERT_EQUAL(STORAGE_BATCH_REQUEST, received_msg.type);

		k_sleep(K_SECONDS(10));

		if (total_samples_received >= total_samples_expected) {
			TEST_ASSERT_EQUAL(STORAGE_BATCH_EMPTY, received_msg.type);
			break;
		}

		TEST_ASSERT_EQUAL(STORAGE_BATCH_AVAILABLE, received_msg.type);

		/* Don't assume batch can fit all remaining data - just verify we got some */
		TEST_ASSERT_GREATER_THAN(0, received_msg.data_len);

		items_read = read_batch_data(received_msg.data_len);

		/* Verify received data matches expected values based on destructive
		 * consumption. Since storage consumes data destructively, we expect
		 * samples in sequence: first iteration gets samples 0-N, second gets
		 * N+1-M, etc.
		 */
		for (size_t i = 0; i < received_battery_samples_count; i++) {
			size_t expected_idx = total_battery_received + i;

			TEST_ASSERT_EQUAL_DOUBLE(battery_samples[expected_idx],
						received_battery_samples[i]);
		}

		for (size_t i = 0; i < received_env_samples_count; i++) {
			size_t expected_idx = total_env_received + i;

			TEST_ASSERT_EQUAL_DOUBLE(env_samples[expected_idx].temperature,
						received_env_samples[i].temperature);
			TEST_ASSERT_EQUAL_DOUBLE(env_samples[expected_idx].humidity,
						received_env_samples[i].humidity);
			TEST_ASSERT_EQUAL_DOUBLE(env_samples[expected_idx].pressure,
						received_env_samples[i].pressure);
		}

		for (size_t i = 0; i < received_location_samples_count; i++) {
			size_t expected_idx = total_location_received + i;

			TEST_ASSERT_EQUAL_DOUBLE(
				location_samples[expected_idx].gnss_data.latitude,
				received_location_samples[i].gnss_data.latitude);
			TEST_ASSERT_EQUAL_DOUBLE(
				location_samples[expected_idx].gnss_data.longitude,
				received_location_samples[i].gnss_data.longitude);
			TEST_ASSERT_EQUAL_FLOAT(
				location_samples[expected_idx].gnss_data.accuracy,
				received_location_samples[i].gnss_data.accuracy);
		}

		/* Update progress tracking */
		total_battery_received += received_battery_samples_count;
		total_env_received += received_env_samples_count;
		total_location_received += received_location_samples_count;
		total_samples_received += items_read;

	} while (received_msg.data_len > 0);

	/* Verify we received all expected samples */
	TEST_ASSERT_EQUAL(num_samples, total_battery_received);
	TEST_ASSERT_EQUAL(num_samples, total_env_received);
	TEST_ASSERT_EQUAL(num_samples, total_location_received);
	TEST_ASSERT_EQUAL(total_samples_expected, total_samples_received);

	/* Final request should return empty */
	err = zbus_chan_pub(&STORAGE_CHAN, &batch_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
	k_sleep(K_SECONDS(1));
	TEST_ASSERT_EQUAL(STORAGE_BATCH_EMPTY, received_msg.type);

	/* Close the batch session */
	close_batch_and_assert(batch_msg.session_id);

	/* Clean up after test */
	err = zbus_chan_pub(&STORAGE_CHAN, &clear_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Allow for storage clear to complete */
	k_sleep(K_SECONDS(1));
}

void test_store_retrieve_location(void)
{
	int err;
	struct location_msg msg = {
		.type = LOCATION_GNSS_DATA,
	};
	struct storage_msg buffer_msg = {
		.type = STORAGE_MODE_BUFFER
	};
	struct storage_msg flush_msg = {
		.type = STORAGE_FLUSH
	};
	/* Calculate how many samples we actually expect to receive */
	const size_t expected_samples =
		MIN(ARRAY_SIZE(location_samples), CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE);
	const size_t start_idx =
		(ARRAY_SIZE(location_samples) > CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE) ?
			(ARRAY_SIZE(location_samples) - CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE) :
			0;
	size_t samples_to_check;

	/* Ensure we're in buffer mode */
	err = zbus_chan_pub(&STORAGE_CHAN, &buffer_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
	k_sleep(K_MSEC(100));

	for (size_t i = 0; i < ARRAY_SIZE(location_samples); i++) {
		msg = location_samples[i];

		/* Store location data */
		err = zbus_chan_pub(&LOCATION_CHAN, &msg, K_SECONDS(1));
		TEST_ASSERT_EQUAL(0, err);
	}

	err = zbus_chan_pub(&STORAGE_CHAN, &flush_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_SECONDS(10));

	/* Only compare data that was actually received */
	samples_to_check = MIN(expected_samples, received_location_samples_count);

	TEST_ASSERT_GREATER_THAN(0, samples_to_check);

	for (size_t i = 0; i < samples_to_check; i++) {
		const size_t sample_idx = start_idx + i;

		/* Verify received data */
		TEST_ASSERT_EQUAL_DOUBLE(location_samples[sample_idx].gnss_data.latitude,
					 received_location_samples[i].gnss_data.latitude);
		TEST_ASSERT_EQUAL_DOUBLE(location_samples[sample_idx].gnss_data.longitude,
					 received_location_samples[i].gnss_data.longitude);
		TEST_ASSERT_EQUAL_FLOAT(location_samples[sample_idx].gnss_data.accuracy,
					received_location_samples[i].gnss_data.accuracy);
	}
}

void test_storage_set_passthrough_mode(void)
{
	int err;
	struct storage_msg passthrough_request = {
		.type = STORAGE_MODE_PASSTHROUGH_REQUEST,
	};

	/* Publish the passthrough request */
	err = zbus_chan_pub(&STORAGE_CHAN, &passthrough_request, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_SECONDS(1));

	/* Should get explicit confirmation */
	TEST_ASSERT_EQUAL(STORAGE_MODE_PASSTHROUGH, received_msg.type);
}

void test_storage_passthrough_data(void)
{
	int err;
	struct storage_msg passthrough_request = {
		.type = STORAGE_MODE_PASSTHROUGH_REQUEST,
	};
	struct power_msg power_msg = {
		.type = POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE,
	};
	struct environmental_msg env_msg = {
		.type = ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE,
		.temperature = 25.0,
		.humidity = 50.0,
		.pressure = 1013.25
	};

	/* Set storage to passthrough mode */
	err = zbus_chan_pub(&STORAGE_CHAN, &passthrough_request, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Wait for mode change confirmation */
	k_sleep(K_SECONDS(1));

	/* Should get explicit confirmation */
	TEST_ASSERT_EQUAL(STORAGE_MODE_PASSTHROUGH, received_msg.type);

	for (int i = 0; i < 10; i++) {
		power_msg.percentage = battery_samples[i];

		err = zbus_chan_pub(&POWER_CHAN, &power_msg, K_SECONDS(1));
		TEST_ASSERT_EQUAL(0, err);

		/* Wait for the message to be processed */
		k_sleep(K_SECONDS(1));

		/* Verify that the received message is of type STORAGE_DATA */
		TEST_ASSERT_EQUAL(STORAGE_DATA, received_msg.type);
		TEST_ASSERT_EQUAL(STORAGE_TYPE_BATTERY, received_msg.data_type);
		TEST_ASSERT_EQUAL_DOUBLE(power_msg.percentage, *(double *)received_msg.buffer);

		env_msg = env_samples[i];
		env_msg.type = ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE;

		err = zbus_chan_pub(&ENVIRONMENTAL_CHAN, &env_msg, K_SECONDS(1));
		TEST_ASSERT_EQUAL(0, err);

		/* Wait for the message to be processed */
		k_sleep(K_SECONDS(1));

		/* Verify that the received message is of type STORAGE_DATA */
		TEST_ASSERT_EQUAL(STORAGE_DATA, received_msg.type);
		TEST_ASSERT_EQUAL(STORAGE_TYPE_ENVIRONMENTAL, received_msg.data_type);
		TEST_ASSERT_EQUAL_DOUBLE(env_msg.temperature,
			((struct environmental_msg *)received_msg.buffer)->temperature);
		TEST_ASSERT_EQUAL_DOUBLE(env_msg.humidity,
			((struct environmental_msg *)received_msg.buffer)->humidity);
		TEST_ASSERT_EQUAL_DOUBLE(env_msg.pressure,
			((struct environmental_msg *)received_msg.buffer)->pressure);
	}
}

void test_storage_batch_error_in_passthrough_mode(void)
{
	int err;
	struct storage_msg passthrough_request = { .type = STORAGE_MODE_PASSTHROUGH_REQUEST };
	struct storage_msg batch_request = {
		.type = STORAGE_BATCH_REQUEST,
		.session_id = 0x44444444
	};

	/* Set storage to passthrough mode */
	err = zbus_chan_pub(&STORAGE_CHAN, &passthrough_request, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Wait for mode change confirmation */
	k_sleep(K_MSEC(100));
	TEST_ASSERT_EQUAL(STORAGE_MODE_PASSTHROUGH, received_msg.type);

	/* Request batch while in passthrough mode - should get ERROR response */
	err = zbus_chan_pub(&STORAGE_CHAN, &batch_request, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Wait for response */
	k_sleep(K_MSEC(100));

	/* Verify we got STORAGE_BATCH_ERROR with correct session_id */
	TEST_ASSERT_EQUAL(STORAGE_BATCH_ERROR, received_msg.type);
	TEST_ASSERT_EQUAL(0x44444444, received_msg.session_id);
}

void test_storage_batch_busy_when_batch_active(void)
{
	int err;
	struct environmental_msg env_msg = { .type = ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE };
	struct storage_msg buffer_msg = { .type = STORAGE_MODE_BUFFER_REQUEST };
	struct storage_msg first_request = {
		.type = STORAGE_BATCH_REQUEST,
		.session_id = 0x55555555
	};
	struct storage_msg second_request = {
		.type = STORAGE_BATCH_REQUEST,
		.session_id = 0x66666666
	};
	struct storage_msg close_msg = {
		.type = STORAGE_BATCH_CLOSE,
		.session_id = 0x55555555
	};

	/* Ensure we're in buffer mode */
	err = zbus_chan_pub(&STORAGE_CHAN, &buffer_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
	k_sleep(K_MSEC(100));

	/* Add some data to storage */
	for (size_t i = 0; i < 5; i++) {
		populate_env_message(i, &env_msg);
		publish_and_assert(&ENVIRONMENTAL_CHAN, &env_msg);
	}

			/* First batch request - should succeed */
	err = zbus_chan_pub(&STORAGE_CHAN, &first_request, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Wait for response */
	k_sleep(K_SECONDS(1));
	TEST_ASSERT_EQUAL(STORAGE_BATCH_AVAILABLE, received_msg.type);
	TEST_ASSERT_EQUAL(0x55555555, received_msg.session_id);

	/* Second batch request while first is active - should get BUSY */
	err = zbus_chan_pub(&STORAGE_CHAN, &second_request, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Wait for response */
	k_sleep(K_MSEC(100));

	/* Verify we got STORAGE_BATCH_BUSY with correct session_id */
	TEST_ASSERT_EQUAL(STORAGE_BATCH_BUSY, received_msg.type);
	TEST_ASSERT_EQUAL(0x66666666, received_msg.session_id);

	/* Clean up - close the first session */
	err = zbus_chan_pub(&STORAGE_CHAN, &close_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
	k_sleep(K_MSEC(100));
}

void test_storage_cannot_change_to_passthrough_while_batch_active(void)
{
	int err;
	struct environmental_msg env_msg = { .type = ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE };
	struct storage_msg buffer_msg = { .type = STORAGE_MODE_BUFFER_REQUEST };
	struct storage_msg passthrough_msg = { .type = STORAGE_MODE_PASSTHROUGH_REQUEST };
	struct storage_msg batch_request = {
		.type = STORAGE_BATCH_REQUEST,
		.session_id = 0x77777777
	};
	struct storage_msg close_msg = {
		.type = STORAGE_BATCH_CLOSE,
		.session_id = 0x77777777
	};
	struct storage_msg passthrough_request = {
		.type = STORAGE_MODE_PASSTHROUGH_REQUEST
	};

	/* Ensure we're in buffer mode */
	err = zbus_chan_pub(&STORAGE_CHAN, &buffer_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_MSEC(100));

	/* Add some data to storage */
	for (size_t i = 0; i < 5; i++) {
		populate_env_message(i, &env_msg);
		publish_and_assert(&ENVIRONMENTAL_CHAN, &env_msg);
	}

	/* Start a batch session */
	err = zbus_chan_pub(&STORAGE_CHAN, &batch_request, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Wait for response */
	k_sleep(K_SECONDS(1));

	TEST_ASSERT_EQUAL(STORAGE_BATCH_AVAILABLE, received_msg.type);
	TEST_ASSERT_EQUAL(0x77777777, received_msg.session_id);

	/* Try to change to passthrough mode while batch is active - should be rejected */
	err = zbus_chan_pub(&STORAGE_CHAN, &passthrough_request, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Wait for explicit rejection response */
	k_sleep(K_MSEC(100));

	/* Should get explicit rejection with reason */
	TEST_ASSERT_EQUAL(STORAGE_MODE_CHANGE_REJECTED, received_msg.type);
	TEST_ASSERT_EQUAL(STORAGE_REJECT_BATCH_ACTIVE, received_msg.reject_reason);

	/* Verify storage is still in batch active state by making another batch request */
	batch_request.session_id = 0x88888888; /* Different session to test busy response */

	err = zbus_chan_pub(&STORAGE_CHAN, &batch_request, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_MSEC(100));

	/* Should get BATCH_BUSY because original session is still active */
	TEST_ASSERT_EQUAL(STORAGE_BATCH_BUSY, received_msg.type);
	TEST_ASSERT_EQUAL(0x88888888, received_msg.session_id);

	/* Clean up - close the session */
	err = zbus_chan_pub(&STORAGE_CHAN, &close_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_MSEC(100));

	/* Now passthrough mode change should work */
	err = zbus_chan_pub(&STORAGE_CHAN, &passthrough_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_SECONDS(1));

	TEST_ASSERT_EQUAL(STORAGE_MODE_PASSTHROUGH, received_msg.type);
}


extern int unity_main(void);

int main(void)
{
	(void)unity_main();

	return 0;
}
