/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <unity.h>

#include <zephyr/fff.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/logging/log.h>

#include "message_channel.h"
#include "gas_sensor.h"

#include "zcbor_decode.h"
#include "env_object_decode.h"

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(int, date_time_now, int64_t *);
FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);

LOG_MODULE_REGISTER(environmental_module_test, 4);

ZBUS_MSG_SUBSCRIBER_DEFINE(transport);
ZBUS_CHAN_ADD_OBS(PAYLOAD_CHAN, transport, 0);

#define FAKE_TIME_MS 1716552398505
#define SENSOR_TEMPERATURE 25.5
#define SENSOR_PRESSURE 100000.0
#define SENSOR_HUMIDITY 50.0
#define SENSOR_IAQ 100
#define SENSOR_CO2 400
#define SENSOR_VOC 100

static const struct device *const sensor_dev = DEVICE_DT_GET(DT_ALIAS(gas_sensor));

static int date_time_now_custom_fake(int64_t *time)
{
	*time = FAKE_TIME_MS;
	return 0;
}

static void send_time_available(void)
{
	enum time_status time_type = TIME_AVAILABLE;
	int err = zbus_chan_pub(&TIME_CHAN, &time_type, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

void send_trigger(void)
{
	enum trigger_type trigger_type = TRIGGER_DATA_SAMPLE;
	int err = zbus_chan_pub(&TRIGGER_CHAN, &trigger_type, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

void wait_for_and_decode_payload(struct env_object *env_object)
{
	const struct zbus_channel *chan;
	static struct payload received_payload;
	int err;

	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_MSEC(100));

	err = zbus_sub_wait_msg(&transport, &chan, &received_payload, K_MSEC(1000));
	if (err == -ENOMSG) {
		LOG_ERR("No payload message received");
		TEST_FAIL();
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	/* check if chan is payload channel */
	if (chan != &PAYLOAD_CHAN) {
		LOG_ERR("Received message from wrong channel");
		TEST_FAIL();
	}

	/* decode payload */
	err = cbor_decode_env_object(received_payload.buffer,
			       received_payload.buffer_len, env_object, NULL);
	if (err != ZCBOR_SUCCESS) {
		LOG_ERR("Failed to decode payload");
		TEST_FAIL();
	}
}

void setUp(void)
{
	struct gas_sensor_dummy_data *data = sensor_dev->data;
	memset(data, 0, sizeof(struct gas_sensor_dummy_data));

	/* reset fakes */
	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(task_wdt_add);
	RESET_FAKE(date_time_now);

	date_time_now_fake.custom_fake = date_time_now_custom_fake;
	send_time_available();
}

void tearDown(void)
{
	const struct zbus_channel *chan;
	static struct payload received_payload;
	int err;

	err = zbus_sub_wait_msg(&transport, &chan, &received_payload, K_MSEC(1000));
	if (err == 0) {
		LOG_ERR("Unhandled message in payload channel");
		TEST_FAIL();
	}
}

void set_temperature(float temperature)
{
	struct gas_sensor_dummy_data *data = sensor_dev->data;
	data->temperature = temperature;
}

void set_pressure(float pressure)
{
	struct gas_sensor_dummy_data *data = sensor_dev->data;
	data->pressure = pressure;
}

void set_humidity(float humidity)
{
	struct gas_sensor_dummy_data *data = sensor_dev->data;
	data->humidity = humidity;
}

void set_iaq(int iaq)
{
	struct gas_sensor_dummy_data *data = sensor_dev->data;
	data->iaq = iaq;
}

void test_only_timestamp(void)
{
	static struct env_object env_object = {0};

	/* Given
	 * Only timestamp needed for before state. Which is handled by date_time_now_fake
	 */

	/* When */
	send_trigger();
	wait_for_and_decode_payload(&env_object);
}

void test_temperaure(void)
{
	static struct env_object env_object = {0};

	/* Given */
	set_temperature(SENSOR_TEMPERATURE);

	/* When */
	send_trigger();
	wait_for_and_decode_payload(&env_object);

	/* Then */
	TEST_ASSERT_EQUAL_FLOAT_MESSAGE(SENSOR_TEMPERATURE, env_object.temperature_m.vf, "temperature");
}

void test_pressure(void)
{
	static struct env_object env_object = {0};

	/* Given */
	set_pressure(SENSOR_PRESSURE);

	/* When */
	send_trigger();
	wait_for_and_decode_payload(&env_object);

	/* Then */
	TEST_ASSERT_EQUAL_FLOAT_MESSAGE(SENSOR_PRESSURE / 100, env_object.pressure_m.vf, "pressure");
}

void test_humidity(void)
{
	static struct env_object env_object = {0};

	/* Given */
	set_humidity(SENSOR_HUMIDITY);

	/* When */
	send_trigger();
	wait_for_and_decode_payload(&env_object);

	/* Then */
	TEST_ASSERT_EQUAL_FLOAT_MESSAGE(SENSOR_HUMIDITY, env_object.humidity_m.vf, "humidity");
}

void test_iaq(void)
{
	static struct env_object env_object = {0};

	/* Given */
	set_iaq(SENSOR_IAQ);

	/* When */
	send_trigger();
	wait_for_and_decode_payload(&env_object);

	/* Then */
	TEST_ASSERT_EQUAL_INT_MESSAGE(SENSOR_IAQ, env_object.iaq_m.vi, "iaq");
}

void test_no_events_on_zbus_until_watchdog_timeout(void)
{
	/* Wait without feeding any events to zbus until watch dog timeout. */
	k_sleep(K_SECONDS(CONFIG_APP_ENVIRONMENTAL_WATCHDOG_TIMEOUT_SECONDS));

	/* Check if the watchdog was fed atleast once.*/
	TEST_ASSERT_GREATER_OR_EQUAL(1, task_wdt_feed_fake.call_count);
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
