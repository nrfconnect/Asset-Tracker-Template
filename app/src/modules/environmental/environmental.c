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
#include <zephyr/smf.h>

#include "app_common.h"
#include "environmental.h"

/* Register log module */
LOG_MODULE_REGISTER(environmental, CONFIG_APP_ENVIRONMENTAL_LOG_LEVEL);

/* Define channels provided by this module */
ZBUS_CHAN_DEFINE(ENVIRONMENTAL_CHAN,
		 struct environmental_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(environmental);

/* Observe channels */
ZBUS_CHAN_ADD_OBS(ENVIRONMENTAL_CHAN, environmental, 0);

#define MAX_MSG_SIZE sizeof(struct environmental_msg)

BUILD_ASSERT(CONFIG_APP_ENVIRONMENTAL_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_ENVIRONMENTAL_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

/* State machine */

/* Environmental module states.
 */
enum environmental_module_state {
	/* The module is running and waiting for sensor value requests */
	STATE_RUNNING,
};

/* State object.
 * Used to transfer context data between state changes.
 */
struct environmental_state_object {
	/* This must be first */
	struct smf_ctx ctx;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;

	/* Buffer for last zbus message */
	uint8_t msg_buf[MAX_MSG_SIZE];

	/* Pointer to the BME680 sensor device */
	const struct device *const bme680;

	/* Sensor values */
	double temperature;
	double pressure;
	double humidity;
};

/* Forward declarations of state handlers */
static enum smf_state_result state_running_run(void *obj);

/* State machine definition */
static const struct smf_state states[] = {
	[STATE_RUNNING] =
		SMF_CREATE_STATE(NULL, state_running_run, NULL, NULL, NULL),
};

static void sample_sensors(const struct device *const bme680)
{
	int err;
	struct sensor_value temp = { 0 };
	struct sensor_value press = { 0 };
	struct sensor_value humidity = { 0 };

	err = sensor_sample_fetch(bme680);
	if (err) {
		LOG_ERR("sensor_sample_fetch, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	err = sensor_channel_get(bme680, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	if (err) {
		LOG_ERR("sensor_channel_get, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	err = sensor_channel_get(bme680, SENSOR_CHAN_PRESS, &press);
	if (err) {
		LOG_ERR("sensor_channel_get, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	err = sensor_channel_get(bme680, SENSOR_CHAN_HUMIDITY, &humidity);
	if (err) {
		LOG_ERR("sensor_channel_get, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	struct environmental_msg msg = {
		.type = ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE,
		.temperature = sensor_value_to_double(&temp),
		.pressure = sensor_value_to_double(&press),
		.humidity = sensor_value_to_double(&humidity),
		.uptime = k_uptime_get(),
	};

	/* Log the environmental values and limit to 2 decimals */
	LOG_DBG("Temperature: %.2f C, Pressure: %.2f Pa, Humidity: %.2f %%",
		msg.temperature, msg.pressure, msg.humidity);

	err = zbus_chan_pub(&ENVIRONMENTAL_CHAN, &msg, K_NO_WAIT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

static void env_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

/* State handlers */

static enum smf_state_result state_running_run(void *obj)
{
	struct environmental_state_object const *state_object = obj;

	if (&ENVIRONMENTAL_CHAN == state_object->chan) {
		struct environmental_msg msg = MSG_TO_ENVIRONMENTAL_MSG(state_object->msg_buf);

		if (msg.type == ENVIRONMENTAL_SENSOR_SAMPLE_REQUEST) {
			LOG_DBG("Environmental values sample request received, getting data");
			sample_sensors(state_object->bme680);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void env_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms =
		(CONFIG_APP_ENVIRONMENTAL_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_ENVIRONMENTAL_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	static struct environmental_state_object environmental_state = {
		.bme680 = DEVICE_DT_GET(DT_NODELABEL(bme680)),
	};

	LOG_DBG("Environmental module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, env_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		SEND_FATAL_ERROR();
		return;
	}

	smf_set_initial(SMF_CTX(&environmental_state), &states[STATE_RUNNING]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = zbus_sub_wait_msg(&environmental,
					&environmental_state.chan,
					environmental_state.msg_buf,
					zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = smf_run_state(SMF_CTX(&environmental_state));
		if (err) {
			LOG_ERR("smf_run_state(), error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}
	}
}

K_THREAD_DEFINE(environmental_module_thread_id,
		CONFIG_APP_ENVIRONMENTAL_THREAD_STACK_SIZE,
		env_module_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
