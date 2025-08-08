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

static void request_fifo_and_assert(void)
{
	int err;
	struct storage_msg request_msg = { .type = STORAGE_FIFO_REQUEST };

	err = zbus_chan_pub(&STORAGE_CHAN, &request_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	TEST_ASSERT_EQUAL(STORAGE_FIFO_REQUEST, received_msg.type);
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

static void read_fifo(struct k_fifo *fifo, size_t item_count)
{
	while (item_count--) {
		struct storage_fifo_item *item = k_fifo_get(fifo, K_NO_WAIT);

		if (item == NULL) {
			printk("FIFO is empty, no more items to read\n");
			return;
		}

		switch (item->type) {
		case STORAGE_TYPE_BATTERY:
			if (received_battery_samples_count < ARRAY_SIZE(received_battery_samples)) {
				received_battery_samples[received_battery_samples_count++] =
					item->data.BATTERY;
			}
			break;
		case STORAGE_TYPE_ENVIRONMENTAL:
			if (received_env_samples_count < ARRAY_SIZE(received_env_samples)) {
				received_env_samples[received_env_samples_count++] =
					item->data.ENVIRONMENTAL;
			}
			break;
		case STORAGE_TYPE_LOCATION:
			if (received_location_samples_count <
			    ARRAY_SIZE(received_location_samples)) {
				received_location_samples[received_location_samples_count++] =
					item->data.LOCATION;
			}
			break;
		default:
			printk("Unknown storage type: %d\n", item->type);
			break;
		}

		item->finished(item);
	}
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

	/* Request FIFO access */
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

void test_storage_fifo_request_empty(void)
{
	int err;
	struct storage_msg msg = {
		.type = STORAGE_FIFO_REQUEST,
	};

	/* Request FIFO data */
	err = zbus_chan_pub(&STORAGE_CHAN, &msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	TEST_ASSERT_EQUAL(STORAGE_FIFO_REQUEST, received_msg.type);

	/* Wait for the FIFO to (not) be populated */
	k_sleep(K_SECONDS(1));

	/* Check if the FIFO is available */
	TEST_ASSERT_EQUAL(STORAGE_FIFO_EMPTY, received_msg.type);
	TEST_ASSERT_EQUAL(0, received_msg.data_len);
}

void test_storage_fifo_request_and_retrieve(void)
{
	int err;
	struct environmental_msg env_msg = { .type = ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE };
	struct storage_msg clear_msg = { .type = STORAGE_CLEAR };
	const uint8_t num_samples = 30;

	for (size_t i = 0; i < num_samples; i++) {
		populate_env_message(i, &env_msg);

		/* Store environmental data */
		publish_and_assert(&ENVIRONMENTAL_CHAN, &env_msg);
	}

	/* Request FIFO data */
	request_fifo_and_assert();

	/* Wait for the FIFO to be populated */
	k_sleep(K_SECONDS(1));

	TEST_ASSERT_EQUAL(STORAGE_FIFO_AVAILABLE, received_msg.type);
	TEST_ASSERT_EQUAL(MIN(CONFIG_APP_STORAGE_FIFO_ITEM_COUNT, num_samples),
			  received_msg.data_len);

	read_fifo(received_msg.fifo, received_msg.data_len);

	for (size_t i = 0; i < received_msg.data_len; i++) {
		TEST_ASSERT_EQUAL_DOUBLE(env_samples[i].temperature,
					 received_env_samples[i].temperature);
		TEST_ASSERT_EQUAL_DOUBLE(env_samples[i].humidity, received_env_samples[i].humidity);
		TEST_ASSERT_EQUAL_DOUBLE(env_samples[i].pressure, received_env_samples[i].pressure);
	}

	/* Clean up after test */
	err = zbus_chan_pub(&STORAGE_CHAN, &clear_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Allow for storage clear to complete */
	k_sleep(K_SECONDS(1));
}

/* The storage backend can hold a total of CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE items per storage
 * data type. The FIFO can hold a maximum of CONFIG_APP_STORAGE_FIFO_ITEM_COUNT items.
 * This test will request the FIFO multiple times to ensure that it can handle multiple requests
 * and that it returns the correct number of items each time, and the correct data, until the
 * storage is empty.
 */
void test_storage_fifo_request_multiple(void)
{
	int err;
	struct environmental_msg env_msg = { .type = ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE };
	struct storage_msg request_msg = { .type = STORAGE_FIFO_REQUEST };
	struct storage_msg clear_msg = { .type = STORAGE_CLEAR };
	const uint8_t num_samples = CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE;
	uint8_t samples_left = num_samples;
	uint8_t recv_samples = 0;

	for (size_t i = 0; i < num_samples; i++) {
		populate_env_message(i, &env_msg);

		/* Store environmental data */
		publish_and_assert(&ENVIRONMENTAL_CHAN, &env_msg);
	}

	while (samples_left) {
		/* Request FIFO data */
		err = zbus_chan_pub(&STORAGE_CHAN, &request_msg, K_SECONDS(1));
		TEST_ASSERT_EQUAL(0, err);

		TEST_ASSERT_EQUAL(STORAGE_FIFO_REQUEST, received_msg.type);

		/* Wait for the FIFO to be populated */
		k_sleep(K_SECONDS(1));

		TEST_ASSERT_EQUAL(STORAGE_FIFO_AVAILABLE, received_msg.type);
		TEST_ASSERT_EQUAL(MIN(CONFIG_APP_STORAGE_FIFO_ITEM_COUNT, samples_left),
				  received_msg.data_len);

		read_fifo(received_msg.fifo, received_msg.data_len);

		for (size_t i = recv_samples; i < (recv_samples + received_msg.data_len); i++) {
			TEST_ASSERT_EQUAL_DOUBLE(env_samples[i].temperature,
						 received_env_samples[i].temperature);
			TEST_ASSERT_EQUAL_DOUBLE(env_samples[i].humidity,
						 received_env_samples[i].humidity);
			TEST_ASSERT_EQUAL_DOUBLE(env_samples[i].pressure,
						 received_env_samples[i].pressure);
		}

		recv_samples += received_msg.data_len;
		samples_left -= received_msg.data_len;
	}

	/* Clean up after test */
	err = zbus_chan_pub(&STORAGE_CHAN, &clear_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Allow for storage clear to complete */
	k_sleep(K_SECONDS(1));
}

void test_storage_fifo_clear_when_empty(void)
{
	int err;
	struct storage_msg clear_fifo = { .type = STORAGE_FIFO_CLEAR };
	struct storage_msg request = { .type = STORAGE_FIFO_REQUEST };

	/* Clear FIFO when it's already empty should be a no-op */
	err = zbus_chan_pub(&STORAGE_CHAN, &clear_fifo, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Request FIFO data -> expect EMPTY */
	err = zbus_chan_pub(&STORAGE_CHAN, &request, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Allow handling */
	k_sleep(K_SECONDS(1));

	TEST_ASSERT_EQUAL(STORAGE_FIFO_EMPTY, received_msg.type);
}

void test_storage_fifo_clear_after_populate(void)
{
	int err;
	struct environmental_msg env_msg =
		{ .type = ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE };
	struct storage_msg request = { .type = STORAGE_FIFO_REQUEST };
	struct storage_msg clear_fifo = { .type = STORAGE_FIFO_CLEAR };

	/* Store some environmental data */
	for (size_t i = 0; i < 5; i++) {
		populate_env_message(i, &env_msg);
		err = zbus_chan_pub(&ENVIRONMENTAL_CHAN, &env_msg, K_SECONDS(1));
		TEST_ASSERT_EQUAL(0, err);
	}

	/* Request FIFO data -> expect AVAILABLE */
	err = zbus_chan_pub(&STORAGE_CHAN, &request, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
	k_sleep(K_SECONDS(1));
	TEST_ASSERT_EQUAL(STORAGE_FIFO_AVAILABLE, received_msg.type);

	/* Clear FIFO without consuming items */
	err = zbus_chan_pub(&STORAGE_CHAN, &clear_fifo, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
	k_sleep(K_SECONDS(1));

	/* Ask again -> backend is empty (items were moved to FIFO), expect EMPTY */
	err = zbus_chan_pub(&STORAGE_CHAN, &request, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
	k_sleep(K_SECONDS(1));
	TEST_ASSERT_EQUAL(STORAGE_FIFO_EMPTY, received_msg.type);
}

void test_storage_fifo_request_mixed_data(void)
{
	int err;
	struct power_msg bat_msg = { .type = POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE };
	struct environmental_msg env_msg = { .type = ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE };
	struct location_msg loc_msg = { .type = LOCATION_GNSS_DATA };
	struct storage_msg fifo_msg = { .type = STORAGE_FIFO_REQUEST };
	struct storage_msg clear_msg = { .type = STORAGE_CLEAR };
	const uint8_t num_samples = 30;
	const uint8_t data_types_count = 3;
	const uint8_t max_fifo_items = CONFIG_APP_STORAGE_FIFO_ITEM_COUNT * data_types_count;
	uint8_t total_samples_expected = num_samples * data_types_count;
	uint8_t total_samples_received = 0;

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
		err = zbus_chan_pub(&STORAGE_CHAN, &fifo_msg, K_SECONDS(1));
		TEST_ASSERT_EQUAL(0, err);

		TEST_ASSERT_EQUAL(STORAGE_FIFO_REQUEST, received_msg.type);

		k_sleep(K_SECONDS(10));

		if (total_samples_received >= total_samples_expected) {
			TEST_ASSERT_EQUAL(STORAGE_FIFO_EMPTY, received_msg.type);
			break;
		}

		TEST_ASSERT_EQUAL(STORAGE_FIFO_AVAILABLE, received_msg.type);

		printk("max_fifo_items: %d, total_samples_expected: %d, "
			"total_samples_received: %d\n",
		       max_fifo_items, total_samples_expected, total_samples_received);
		TEST_ASSERT_EQUAL(
				  MIN(max_fifo_items,
				      total_samples_expected - total_samples_received),
				received_msg.data_len);

		read_fifo(received_msg.fifo, received_msg.data_len);

		/* Since we stored the first num_samples from each array and storage capacity
		 * is sufficient to hold all of them, we compare directly with the indices
		 * that were actually stored (0 to num_samples-1)
		 */
		for (size_t i = 0; i < received_battery_samples_count; i++) {
			TEST_ASSERT_EQUAL_DOUBLE(battery_samples[i], received_battery_samples[i]);
		}

		for (size_t i = 0; i < received_env_samples_count; i++) {
			TEST_ASSERT_EQUAL_DOUBLE(env_samples[i].temperature,
						received_env_samples[i].temperature);
			TEST_ASSERT_EQUAL_DOUBLE(env_samples[i].humidity,
						received_env_samples[i].humidity);
			TEST_ASSERT_EQUAL_DOUBLE(env_samples[i].pressure,
						received_env_samples[i].pressure);
		}

		for (size_t i = 0; i < received_location_samples_count; i++) {
			TEST_ASSERT_EQUAL_DOUBLE(location_samples[i].gnss_data.latitude,
					received_location_samples[i].gnss_data.latitude);
			TEST_ASSERT_EQUAL_DOUBLE(location_samples[i].gnss_data.longitude,
					received_location_samples[i].gnss_data.longitude);
			TEST_ASSERT_EQUAL_FLOAT(location_samples[i].gnss_data.accuracy,
					received_location_samples[i].gnss_data.accuracy);
		}

		total_samples_received += received_msg.data_len;
	} while (received_msg.data_len > 0);

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
	struct storage_msg flush_msg = {
		.type = STORAGE_FLUSH
	};

	for (size_t i = 0; i < ARRAY_SIZE(location_samples); i++) {
		msg = location_samples[i];

		/* Store location data */
		err = zbus_chan_pub(&LOCATION_CHAN, &msg, K_SECONDS(1));
		TEST_ASSERT_EQUAL(0, err);
	}

	/* Request FIFO access */
	err = zbus_chan_pub(&STORAGE_CHAN, &flush_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_SECONDS(10));

	/* Calculate how many samples we actually expect to receive */
	const size_t expected_samples =
		MIN(ARRAY_SIZE(location_samples), CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE);
	const size_t start_idx =
		(ARRAY_SIZE(location_samples) > CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE) ?
			(ARRAY_SIZE(location_samples) - CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE) :
			0;

	for (size_t i = 0; i < expected_samples; i++) {
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
	struct storage_msg passthrough_msg = {
		.type = STORAGE_MODE_PASSTHROUGH,
	};

	/* Publish the pass-through message */
	err = zbus_chan_pub(&STORAGE_CHAN, &passthrough_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_SECONDS(1));

	TEST_ASSERT_EQUAL(STORAGE_MODE_PASSTHROUGH, received_msg.type);
}

void test_storage_passthrough_data(void)
{
	int err;
	struct storage_msg pass_through_msg = {
		.type = STORAGE_MODE_PASSTHROUGH,
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

	/* Set storage to pass-through mode */
	err = zbus_chan_pub(&STORAGE_CHAN, &pass_through_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

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

extern int unity_main(void);

int main(void)
{
	(void)unity_main();

	return 0;
}
