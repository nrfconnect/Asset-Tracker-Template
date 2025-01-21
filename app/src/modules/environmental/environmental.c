/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <drivers/bme68x_iaq.h>
#include <date_time.h>

#include "message_channel.h"
#include "modules_common.h"

/* Register log module */
LOG_MODULE_REGISTER(environmental_module, CONFIG_APP_ENVIRONMENTAL_LOG_LEVEL);

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(environmental);

/* Observe trigger channel */
ZBUS_CHAN_ADD_OBS(TRIGGER_CHAN, environmental, 0);
ZBUS_CHAN_ADD_OBS(TIME_CHAN, environmental, 0);

#define MAX_MSG_SIZE (MAX(sizeof(enum trigger_type), sizeof(enum time_status)))

BUILD_ASSERT(CONFIG_APP_ENVIRONMENTAL_WATCHDOG_TIMEOUT_SECONDS >
			CONFIG_APP_ENVIRONMENTAL_EXEC_TIME_SECONDS_MAX,
			"Watchdog timeout must be greater than maximum execution time");

static const struct device *const sensor_dev = DEVICE_DT_GET(DT_ALIAS(gas_sensor));

static void task_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void sample(void)
{
	struct sensor_value temp = { 0 };
	struct sensor_value press = { 0 };
	struct sensor_value humidity = { 0 };
	struct sensor_value iaq = { 0 };
	struct sensor_value co2 = { 0 };
	struct sensor_value voc = { 0 };
	int ret;

	ret = sensor_sample_fetch(sensor_dev);
	__ASSERT_NO_MSG(ret == 0);
	ret = sensor_channel_get(sensor_dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	__ASSERT_NO_MSG(ret == 0);
	ret = sensor_channel_get(sensor_dev, SENSOR_CHAN_PRESS, &press);
	__ASSERT_NO_MSG(ret == 0);
	ret = sensor_channel_get(sensor_dev, SENSOR_CHAN_HUMIDITY, &humidity);
	__ASSERT_NO_MSG(ret == 0);
	ret = sensor_channel_get(sensor_dev, SENSOR_CHAN_IAQ, &iaq);
	__ASSERT_NO_MSG(ret == 0);
	ret = sensor_channel_get(sensor_dev, SENSOR_CHAN_CO2, &co2);
	__ASSERT_NO_MSG(ret == 0);
	ret = sensor_channel_get(sensor_dev, SENSOR_CHAN_VOC, &voc);
	__ASSERT_NO_MSG(ret == 0);

	LOG_DBG("temp: %d.%06d; press: %d.%06d; humidity: %d.%06d; iaq: %d; CO2: %d.%06d; "
		"VOC: %d.%06d",
		temp.val1, temp.val2, press.val1, press.val2, humidity.val1, humidity.val2,
		iaq.val1, co2.val1, co2.val2, voc.val1, voc.val2);

	/* No further use of the environmental data is implemented */
}

static void environmental_task(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = (CONFIG_APP_ENVIRONMENTAL_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms = (CONFIG_APP_ENVIRONMENTAL_EXEC_TIME_SECONDS_MAX * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	const struct zbus_channel *chan;
	enum trigger_type trigger_type;

	/* Buffer for last zbus message */
	uint8_t msg_buf[MAX_MSG_SIZE];

	LOG_DBG("Environmental module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, task_wdt_callback, (void *)k_current_get());

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = zbus_sub_wait_msg(&environmental, &chan, msg_buf, zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		trigger_type = MSG_TO_TRIGGER_TYPE(msg_buf);

		if (trigger_type == TRIGGER_DATA_SAMPLE) {
			LOG_DBG("Data sample trigger received, getting environmental data");
			sample();
		}
	}
}

K_THREAD_DEFINE(environmental_task_id,
		CONFIG_APP_ENVIRONMENTAL_THREAD_STACK_SIZE,
		environmental_task, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
