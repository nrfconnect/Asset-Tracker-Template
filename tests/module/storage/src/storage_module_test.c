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
		 enum location_msg_type,
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
ZBUS_CHAN_ADD_OBS(POWER_CHAN, power_test_listener, 0);
ZBUS_CHAN_ADD_OBS(ENVIRONMENTAL_CHAN, environmental_test_listener, 0);
ZBUS_CHAN_ADD_OBS(LOCATION_CHAN, location_test_listener, 0);

// /* Test data */
static const double battery_samples[] = {
	85.5, 85.2, 85.0, 84.8, 84.5, 84.2, 84.0, 83.8, 83.5, 83.2,
	83.0, 82.8, 82.5, 82.2, 82.0, 81.8, 81.5, 81.2, 81.0, 80.8,
	80.5, 80.2, 80.0, 79.8, 79.5, 79.2, 79.0, 78.8, 78.5, 78.2,
	78.0, 77.8, 77.5, 77.2, 77.0, 76.8, 76.5, 76.2, 76.0, 75.8,
	75.5, 75.2, 75.0, 74.8, 74.5, 74.2, 74.0, 73.8, 73.5, 73.2,
	73.0, 72.8, 72.5, 72.2, 72.0, 71.8, 71.5, 71.2, 71.0, 70.8,
	70.5, 70.2, 70.0, 69.8, 69.5, 69.2, 69.0, 68.8, 68.5, 68.2,
	68.0, 67.8, 67.5, 67.2, 67.0, 66.8, 66.5, 66.2, 66.0, 65.8,
	65.5, 65.2, 65.0, 64.8, 64.5, 64.2, 64.0, 63.8, 63.5, 63.2,
	63.0, 62.8, 62.5, 62.2, 62.0, 61.8, 61.5, 61.2, 61.0, 60.8,
};

static const struct environmental_msg env_samples[] = {
	{.temperature = 0.0, .humidity = 50.0, .pressure = 1013.25},
	{.temperature = 1.0, .humidity = 55.0, .pressure = 1012.75},
	{.temperature = 2.0, .humidity = 60.0, .pressure = 1012.25},
	{.temperature = 3.0, .humidity = 65.0, .pressure = 1011.75},
	{.temperature = 4.0, .humidity = 70.0, .pressure = 1011.25},
	{.temperature = 5.0, .humidity = 75.0, .pressure = 1010.75},
	{.temperature = 6.0, .humidity = 80.0, .pressure = 1010.25},
	{.temperature = 7.0, .humidity = 85.0, .pressure = 1009.75},
	{.temperature = 8.0, .humidity = 90.0, .pressure = 1009.25},
	{.temperature = 9.0, .humidity = 95.0, .pressure = 1008.75},
	{.temperature = 10.0, .humidity = 100.0, .pressure = 1008.25},
	{.temperature = 11.0, .humidity = 105.0, .pressure = 1007.75},
	{.temperature = 12.0, .humidity = 110.0, .pressure = 1007.25},
	{.temperature = 13.0, .humidity = 115.0, .pressure = 1006.75},
	{.temperature = 14.0, .humidity = 120.0, .pressure = 1006.25},
	{.temperature = 15.0, .humidity = 125.0, .pressure = 1005.75},
	{.temperature = 16.0, .humidity = 130.0, .pressure = 1005.25},
	{.temperature = 17.0, .humidity = 135.0, .pressure = 1004.75},
	{.temperature = 18.0, .humidity = 140.0, .pressure = 1004.25},
	{.temperature = 19.0, .humidity = 145.0, .pressure = 1003.75},
	{.temperature = 20.0, .humidity = 45.0, .pressure = 1013.75},
	{.temperature = 21.0, .humidity = 40.0, .pressure = 1013.50},
	{.temperature = 22.0, .humidity = 35.0, .pressure = 1013.00},
	{.temperature = 23.0, .humidity = 30.0, .pressure = 1012.50},
	{.temperature = 24.0, .humidity = 25.0, .pressure = 1012.00},
	{.temperature = 25.0, .humidity = 50.0, .pressure = 1013.25},
	{.temperature = 26.0, .humidity = 55.0, .pressure = 1012.75},
	{.temperature = 27.0, .humidity = 60.0, .pressure = 1012.25},
	{.temperature = 28.0, .humidity = 65.0, .pressure = 1011.75},
	{.temperature = 29.0, .humidity = 70.0, .pressure = 1011.25},
	{.temperature = 30.0, .humidity = 75.0, .pressure = 1010.75},
	{.temperature = 31.0, .humidity = 80.0, .pressure = 1010.25},
	{.temperature = 32.0, .humidity = 85.0, .pressure = 1009.75},
	{.temperature = 33.0, .humidity = 90.0, .pressure = 1009.25},
	{.temperature = 34.0, .humidity = 95.0, .pressure = 1008.75},
	{.temperature = 35.0, .humidity = 100.0, .pressure = 1008.25},
	{.temperature = 36.0, .humidity = 105.0, .pressure = 1007.75},
	{.temperature = 37.0, .humidity = 110.0, .pressure = 1007.25},
	{.temperature = 38.0, .humidity = 115.0, .pressure = 1006.75},
	{.temperature = 39.0, .humidity = 120.0, .pressure = 1006.25},
	{.temperature = 40.0, .humidity = 125.0, .pressure = 1005.75},
	{.temperature = 41.0, .humidity = 130.0, .pressure = 1005.25},
	{.temperature = 42.0, .humidity = 135.0, .pressure = 1004.75},
	{.temperature = 43.0, .humidity = 140.0, .pressure = 1004.25},
	{.temperature = 44.0, .humidity = 145.0, .pressure = 1003.75},
	{.temperature = 45.0, .humidity = 150.0, .pressure = 1003.25},
	{.temperature = 46.0, .humidity = 155.0, .pressure = 1002.75},
	{.temperature = 47.0, .humidity = 160.0, .pressure = 1002.25},
	{.temperature = 48.0, .humidity = 165.0, .pressure = 1001.75},
	{.temperature = 49.0, .humidity = 170.0, .pressure = 1001.25},
	{.temperature = 50.0, .humidity = 175.0, .pressure = 1000.75},
	{.temperature = 51.0, .humidity = 180.0, .pressure = 1000.25},
	{.temperature = 52.0, .humidity = 185.0, .pressure = 999.75},
	{.temperature = 53.0, .humidity = 190.0, .pressure = 999.25},
	{.temperature = 54.0, .humidity = 195.0, .pressure = 998.75},
	{.temperature = 55.0, .humidity = 200.0, .pressure = 998.25},
	{.temperature = 56.0, .humidity = 205.0, .pressure = 997.75},
	{.temperature = 57.0, .humidity = 210.0, .pressure = 997.25},
	{.temperature = 58.0, .humidity = 215.0, .pressure = 996.75},
	{.temperature = 59.0, .humidity = 220.0, .pressure = 996.25},
	{.temperature = 60.0, .humidity = 225.0, .pressure = 995.75},
	{.temperature = 61.0, .humidity = 230.0, .pressure = 995.25},
	{.temperature = 62.0, .humidity = 235.0, .pressure = 994.75},
	{.temperature = 63.0, .humidity = 240.0, .pressure = 994.25},
	{.temperature = 64.0, .humidity = 245.0, .pressure = 993.75},
	{.temperature = 65.0, .humidity = 250.0, .pressure = 993.25},
	{.temperature = 66.0, .humidity = 255.0, .pressure = 992.75},
	{.temperature = 67.0, .humidity = 260.0, .pressure = 992.25},
	{.temperature = 68.0, .humidity = 265.0, .pressure = 991.75},
	{.temperature = 69.0, .humidity = 270.0, .pressure = 991.25},
	{.temperature = 70.0, .humidity = 275.0, .pressure = 990.75},
	{.temperature = 71.0, .humidity = 280.0, .pressure = 990.25},
	{.temperature = 72.0, .humidity = 285.0, .pressure = 989.75},
	{.temperature = 73.0, .humidity = 290.0, .pressure = 989.25},
	{.temperature = 74.0, .humidity = 295.0, .pressure = 988.75},
	{.temperature = 75.0, .humidity = 300.0, .pressure = 988.25},
	{.temperature = 76.0, .humidity = 305.0, .pressure = 987.75},
	{.temperature = 77.0, .humidity = 310.0, .pressure = 987.25},
	{.temperature = 78.0, .humidity = 315.0, .pressure = 986.75},
	{.temperature = 79.0, .humidity = 320.0, .pressure = 986.25},
	{.temperature = 80.0, .humidity = 325.0, .pressure = 985.75},
	{.temperature = 81.0, .humidity = 330.0, .pressure = 985.25},
	{.temperature = 82.0, .humidity = 335.0, .pressure = 984.75},
	{.temperature = 83.0, .humidity = 340.0, .pressure = 984.25},
	{.temperature = 84.0, .humidity = 345.0, .pressure = 983.75},
	{.temperature = 85.0, .humidity = 350.0, .pressure = 983.25},
	{.temperature = 86.0, .humidity = 355.0, .pressure = 982.75},
	{.temperature = 87.0, .humidity = 360.0, .pressure = 982.25},
	{.temperature = 88.0, .humidity = 365.0, .pressure = 981.75},
	{.temperature = 89.0, .humidity = 370.0, .pressure = 981.25},
	{.temperature = 90.0, .humidity = 375.0, .pressure = 980.75},
	{.temperature = 91.0, .humidity = 380.0, .pressure = 980.25},
	{.temperature = 92.0, .humidity = 385.0, .pressure = 979.75},
	{.temperature = 93.0, .humidity = 390.0, .pressure = 979.25},
	{.temperature = 94.0, .humidity = 395.0, .pressure = 978.75},
	{.temperature = 95.0, .humidity = 400.0, .pressure = 978.25},
	{.temperature = 96.0, .humidity = 405.0, .pressure = 977.75},
	{.temperature = 97.0, .humidity = 410.0, .pressure = 977.25},
	{.temperature = 98.0, .humidity = 415.0, .pressure = 976.75},
	{.temperature = 99.0, .humidity = 420.0, .pressure = 976.25},
	{.temperature = 100.0, .humidity = 425.0, .pressure = 975.75},
	{.temperature = 101.0, .humidity = 430.0, .pressure = 975.25},
	{.temperature = 102.0, .humidity = 435.0, .pressure = 974.75},
	{.temperature = 103.0, .humidity = 440.0, .pressure = 974.25},
	{.temperature = 104.0, .humidity = 445.0, .pressure = 973.75},
	{.temperature = 105.0, .humidity = 450.0, .pressure = 973.25},
};

static const enum location_msg_type location_samples[] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
	10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
	20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
	30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
	40, 41, 42, 43, 44, 45, 46, 47, 48, 49,
	50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
	60, 61, 62, 63, 64, 65, 66, 67, 68, 69,
	70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
	80, 81, 82, 83, 84, 85, 86, 87, 88, 89,
	90, 91, 92, 93, 94, 95, 96, 97, 98, 99,
	100, 101, 102, 103, 104, 105, 106, 107, 108, 109,
};

static double received_battery_samples[ARRAY_SIZE(battery_samples)];
static uint8_t received_battery_samples_count = 0;

static struct environmental_msg received_env_samples[ARRAY_SIZE(env_samples)];
static uint8_t received_env_samples_count = 0;

static enum location_msg_type received_location_samples[ARRAY_SIZE(location_samples)];
static uint8_t received_location_samples_count = 0;

/* Variables to store received data */
static struct storage_msg received_msg;

static void dummy_cb(const struct zbus_channel *chan)
{
}

static void storage_chan_cb(const struct zbus_channel *chan)
{
	const struct storage_msg *msg = zbus_chan_const_msg(chan);

	if (chan != &STORAGE_CHAN) {
		return;
	}

	received_msg = *msg;

	if (msg->type == STORAGE_DATA) {
		switch (msg->data_type) {
		case STORAGE_TYPE_BATTERY:
			received_battery_samples[received_battery_samples_count++] =
				*(double *)msg->buffer;
			break;
		case STORAGE_TYPE_ENVIRONMENTAL:
			received_env_samples[received_env_samples_count++] =
				*(struct environmental_msg *)msg->buffer;
			break;
		case STORAGE_TYPE_LOCATION:
			received_location_samples[received_location_samples_count++] =
				*(enum location_msg_type *)msg->buffer;
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
		struct storage_data_chunk *chunk = k_fifo_get(fifo, K_NO_WAIT);

		if (chunk == NULL) {
			printk("FIFO is empty, no more items to read\n");
			return;
		}

		switch (chunk->type) {
		case STORAGE_TYPE_BATTERY:
			if (received_battery_samples_count < ARRAY_SIZE(received_battery_samples)) {
				received_battery_samples[received_battery_samples_count++] =
					chunk->data.BATTERY;
			}
			break;
		case STORAGE_TYPE_ENVIRONMENTAL:
			if (received_env_samples_count < ARRAY_SIZE(received_env_samples)) {
				received_env_samples[received_env_samples_count++] =
					chunk->data.ENVIRONMENTAL;
			}
			break;
		case STORAGE_TYPE_LOCATION:
			if (received_location_samples_count < ARRAY_SIZE(received_location_samples)) {
				received_location_samples[received_location_samples_count++] =
					chunk->data.LOCATION;
			}
			break;
		default:
			printk("Unknown storage type: %d\n", chunk->type);
			break;
		}

		chunk->finished(chunk);
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

	/* Request data flush */
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

void test_store_retrieve_location(void)
{
	int err;
	enum location_msg_type loc_msg = LOCATION_SEARCH_STARTED;
	struct storage_msg flush_msg = {
		.type = STORAGE_FLUSH
	};

	for (size_t i = 0; i < ARRAY_SIZE(location_samples); i++) {
		loc_msg = location_samples[i];

		/* Store location data */
		err = zbus_chan_pub(&LOCATION_CHAN, &loc_msg, K_SECONDS(1));
		TEST_ASSERT_EQUAL(0, err);
	}

	err = zbus_chan_pub(&STORAGE_CHAN, &flush_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_SECONDS(10));

	for (size_t i = 0; i < CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE; i++) {
		const size_t sample_idx =
			(ARRAY_SIZE(location_samples) - CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE) + i;

		TEST_ASSERT_EQUAL(location_samples[sample_idx], received_location_samples[i]);
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
		env_msg.temperature = env_samples[i].temperature;
		env_msg.humidity = env_samples[i].humidity;
		env_msg.pressure = env_samples[i].pressure;

		/* Store environmental data */
		err = zbus_chan_pub(&ENVIRONMENTAL_CHAN, &env_msg, K_SECONDS(1));
		TEST_ASSERT_EQUAL(0, err);
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
	enum location_msg_type loc_msg = LOCATION_SEARCH_STARTED;
	struct storage_msg flush_msg = {
		.type = STORAGE_FLUSH
	};
	const uint8_t num_samples = CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE;

	for (size_t i = 0; i < num_samples; i++) {
		bat_msg.percentage = battery_samples[i];
		env_msg.temperature = env_samples[i].temperature;
		env_msg.humidity = env_samples[i].humidity;
		env_msg.pressure = env_samples[i].pressure;
		loc_msg = location_samples[i];

		err = zbus_chan_pub(&LOCATION_CHAN, &loc_msg, K_SECONDS(1));
		TEST_ASSERT_EQUAL(0, err);

		/* Store battery data */
		err = zbus_chan_pub(&POWER_CHAN, &bat_msg, K_SECONDS(1));
		TEST_ASSERT_EQUAL(0, err);

		/* Store environmental data */
		err = zbus_chan_pub(&ENVIRONMENTAL_CHAN, &env_msg, K_SECONDS(1));
		TEST_ASSERT_EQUAL(0, err);
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
		/* Calculate the index in the original sample arrays based on the actual count
		 * of received samples, ensuring we compare with the correct expected samples
		 */
		size_t bat_sample_idx = MAX(num_samples - received_battery_samples_count, 0) + i;
		size_t env_sample_idx = MAX(num_samples - received_env_samples_count, 0) + i;
		size_t loc_sample_idx = MAX(num_samples - received_location_samples_count, 0) + i;

		/* Only perform assertions for samples that were actually received */
		if (i < received_battery_samples_count) {
			TEST_ASSERT_EQUAL_DOUBLE(battery_samples[bat_sample_idx],
						 received_battery_samples[i]);
		}

		if (i < received_env_samples_count) {
			TEST_ASSERT_EQUAL_DOUBLE(env_samples[env_sample_idx].temperature,
						 received_env_samples[i].temperature);
			TEST_ASSERT_EQUAL_DOUBLE(env_samples[env_sample_idx].humidity,
						 received_env_samples[i].humidity);
			TEST_ASSERT_EQUAL_DOUBLE(env_samples[env_sample_idx].pressure,
						 received_env_samples[i].pressure);
		}

		if (i < received_location_samples_count) {
			TEST_ASSERT_EQUAL(location_samples[loc_sample_idx],
					  received_location_samples[i]);
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

void test_storage_fifo_request_populated(void)
{
	int err;
	struct environmental_msg env_msg = { .type = ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE };
	struct storage_msg request_msg = { .type = STORAGE_FIFO_REQUEST };
	struct storage_msg purge_msg = { .type = STORAGE_PURGE };
	const uint8_t num_samples = 30;

	for (size_t i = 0; i < num_samples; i++) {
		env_msg.temperature = env_samples[i].temperature;
		env_msg.humidity = env_samples[i].humidity;
		env_msg.pressure = env_samples[i].pressure;

		/* Store environmental data */
		err = zbus_chan_pub(&ENVIRONMENTAL_CHAN, &env_msg, K_SECONDS(1));
		TEST_ASSERT_EQUAL(0, err);
	}

	/* Request FIFO data */
	err = zbus_chan_pub(&STORAGE_CHAN, &request_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	TEST_ASSERT_EQUAL(STORAGE_FIFO_REQUEST, received_msg.type);

	/* Wait for the FIFO to be populated */
	k_sleep(K_SECONDS(10));

	TEST_ASSERT_EQUAL(STORAGE_FIFO_AVAILABLE, received_msg.type);
	TEST_ASSERT_EQUAL(MIN(CONFIG_APP_STORAGE_FIFO_ITEM_COUNT, num_samples),
			  received_msg.data_len);

	/* Clean up after test */
	err = zbus_chan_pub(&STORAGE_CHAN, &purge_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Allow for storage purge to complete */
	k_sleep(K_SECONDS(1));
}

void test_storage_fifo_request_and_retrieve(void)
{
	int err;
	struct environmental_msg env_msg = { .type = ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE };
	struct storage_msg request_msg = { .type = STORAGE_FIFO_REQUEST };
	struct storage_msg purge_msg = { .type = STORAGE_PURGE };
	const uint8_t num_samples = 30;

	for (size_t i = 0; i < num_samples; i++) {
		env_msg.temperature = env_samples[i].temperature;
		env_msg.humidity = env_samples[i].humidity;
		env_msg.pressure = env_samples[i].pressure;

		/* Store environmental data */
		err = zbus_chan_pub(&ENVIRONMENTAL_CHAN, &env_msg, K_SECONDS(1));
		TEST_ASSERT_EQUAL(0, err);
	}

	/* Request FIFO data */
	err = zbus_chan_pub(&STORAGE_CHAN, &request_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	TEST_ASSERT_EQUAL(STORAGE_FIFO_REQUEST, received_msg.type);

	/* Wait for the FIFO to be populated */
	k_sleep(K_SECONDS(1));

	TEST_ASSERT_EQUAL(STORAGE_FIFO_AVAILABLE, received_msg.type);
	TEST_ASSERT_EQUAL(MIN(CONFIG_APP_STORAGE_FIFO_ITEM_COUNT, num_samples),
			  received_msg.data_len);

	read_fifo(received_msg.fifo, received_msg.data_len);

	for (size_t i = 0; i < received_msg.data_len; i++) {
		TEST_ASSERT_EQUAL_DOUBLE(env_samples[i].temperature, received_env_samples[i].temperature);
		TEST_ASSERT_EQUAL_DOUBLE(env_samples[i].humidity, received_env_samples[i].humidity);
		TEST_ASSERT_EQUAL_DOUBLE(env_samples[i].pressure, received_env_samples[i].pressure);
	}

	/* Clean up after test */
	err = zbus_chan_pub(&STORAGE_CHAN, &purge_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Allow for storage purge to complete */
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
	struct storage_msg purge_msg = { .type = STORAGE_PURGE };
	const uint8_t num_samples = CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE;
	uint8_t samples_left = num_samples;
	uint8_t recv_samples = 0;

	for (size_t i = 0; i < num_samples; i++) {
		env_msg.temperature = env_samples[i].temperature;
		env_msg.humidity = env_samples[i].humidity;
		env_msg.pressure = env_samples[i].pressure;

		/* Store environmental data */
		err = zbus_chan_pub(&ENVIRONMENTAL_CHAN, &env_msg, K_SECONDS(1));
		TEST_ASSERT_EQUAL(0, err);
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
	err = zbus_chan_pub(&STORAGE_CHAN, &purge_msg, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	/* Allow for storage purge to complete */
	k_sleep(K_SECONDS(1));
}

extern int unity_main(void);

int main(void)
{
	(void)unity_main();

	return 0;
}
