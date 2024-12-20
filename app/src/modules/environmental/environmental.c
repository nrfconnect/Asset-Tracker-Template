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
#include <zephyr/smf.h>

#include "message_channel.h"
#include "modules_common.h"
#include "env_object_encode.h"

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

/* Forward declarations */
static struct s_object s_obj;
static void sample(void);

/* State machine */

/* Defininig the module states.
 *
 * STATE_INIT: The environmental module is initializing and waiting for time to be available.
 * STATE_SAMPLING: The environmental module is ready to sample upon receiving a trigger.
 */
enum environmental_module_state {
	STATE_INIT,
	STATE_SAMPLING,
};

/* User defined state object.
 * Used to transfer data between state changes.
 */
struct s_object {
	/* This must be first */
	struct smf_ctx ctx;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;

	/* Buffer for last zbus message */
	uint8_t msg_buf[MAX_MSG_SIZE];
};

/* Forward declarations of state handlers */
static void state_init_run(void *o);
static void state_sampling_run(void *o);

static struct s_object s_obj;
static const struct smf_state states[] = {
	[STATE_INIT] =
		SMF_CREATE_STATE(NULL, state_init_run, NULL,
				 NULL,	/* No parent state */
				 NULL), /* No initial transition */
	[STATE_SAMPLING] =
		SMF_CREATE_STATE(NULL, state_sampling_run, NULL,
				 NULL,
				 NULL),
};

/* State handlers */

static void state_init_run(void *o)
{
	struct s_object *state_object = o;

	if (&TIME_CHAN == state_object->chan) {
		enum time_status time_status = MSG_TO_TIME_STATUS(state_object->msg_buf);

		if (time_status == TIME_AVAILABLE) {
			LOG_DBG("Time available, sampling can start");

			STATE_SET(STATE_SAMPLING);
		}
	}
}

static void state_sampling_run(void *o)
{
	struct s_object *state_object = o;

	if (&TRIGGER_CHAN == state_object->chan) {
		enum trigger_type trigger_type = MSG_TO_TRIGGER_TYPE(state_object->msg_buf);

		if (trigger_type == TRIGGER_DATA_SAMPLE) {
			LOG_DBG("Data sample trigger received, getting environmental data");
			sample();
		}
	}
}

/* End of state handling */

static void task_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void sample(void)
{
	int64_t system_time;
	struct payload payload = { 0 };
	struct sensor_value temp = { 0 };
	struct sensor_value press = { 0 };
	struct sensor_value humidity = { 0 };
	struct sensor_value iaq = { 0 };
	struct sensor_value co2 = { 0 };
	struct sensor_value voc = { 0 };
	struct env_object env_obj = { 0 };
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

	ret = date_time_now(&system_time);
	if (ret) {
		LOG_ERR("Failed to convert uptime to unix time, error: %d", ret);
		return;
	}

	env_obj.temperature_m.bt = (int32_t)(system_time / 1000);
	env_obj.temperature_m.vf = sensor_value_to_double(&temp);
	env_obj.humidity_m.vf = sensor_value_to_double(&humidity);
	env_obj.pressure_m.vf = sensor_value_to_double(&press) / 100;
	env_obj.iaq_m.vi = iaq.val1;

	ret = cbor_encode_env_object(payload.buffer, sizeof(payload.buffer),
				     &env_obj, &payload.buffer_len);
	if (ret) {
		LOG_ERR("Failed to encode env object, error: %d", ret);
		SEND_FATAL_ERROR();
		return;
	}

	LOG_DBG("Submitting payload");

	int err = zbus_chan_pub(&PAYLOAD_CHAN, &payload, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

static void environmental_task(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = (CONFIG_APP_ENVIRONMENTAL_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms = (CONFIG_APP_ENVIRONMENTAL_EXEC_TIME_SECONDS_MAX * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);

	LOG_DBG("Environmental module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, task_wdt_callback, (void *)k_current_get());

	STATE_SET_INITIAL(STATE_INIT);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = zbus_sub_wait_msg(&environmental, &s_obj.chan, s_obj.msg_buf, zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = STATE_RUN();
		if (err) {
			LOG_ERR("handle_message, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}
	}
}

K_THREAD_DEFINE(environmental_task_id,
		CONFIG_APP_ENVIRONMENTAL_THREAD_STACK_SIZE,
		environmental_task, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
