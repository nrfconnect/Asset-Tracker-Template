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

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(int, date_time_now, int64_t *);
FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);

LOG_MODULE_REGISTER(environmental_module_test, 4);

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

void wait_for_and_decode_payload(void)
{
	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_MSEC(100));
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
	k_sleep(K_MSEC(100));
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
	/* Given
	 * Only timestamp needed for before state. Which is handled by date_time_now_fake
	 */

	/* When */
	send_trigger();
	wait_for_and_decode_payload();
}

void test_temperaure(void)
{
	/* Given */
	set_temperature(SENSOR_TEMPERATURE);

	/* When */
	send_trigger();
	wait_for_and_decode_payload();
}

void test_pressure(void)
{
	/* Given */
	set_pressure(SENSOR_PRESSURE);

	/* When */
	send_trigger();
	wait_for_and_decode_payload();

	/* Then */
}

void test_humidity(void)
{
	/* Given */
	set_humidity(SENSOR_HUMIDITY);

	/* When */
	send_trigger();
	wait_for_and_decode_payload();

	/* Then */
}

void test_iaq(void)
{
	/* Given */
	set_iaq(SENSOR_IAQ);

	/* When */
	send_trigger();
	wait_for_and_decode_payload();

	/* Then */
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
