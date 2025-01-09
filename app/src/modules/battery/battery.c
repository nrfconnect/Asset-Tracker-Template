/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/drivers/sensor/npm1300_charger.h>
#include <zephyr/sys/util.h>
#include <nrf_fuel_gauge.h>
#include <date_time.h>
#include <math.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/smf.h>

#include "lp803448_model.h"
#include "message_channel.h"
#include "modules_common.h"

/* Register log module */
LOG_MODULE_REGISTER(battery, CONFIG_APP_BATTERY_LOG_LEVEL);

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(battery);

/* Observe channels */
ZBUS_CHAN_ADD_OBS(TRIGGER_CHAN, battery, 0);
ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, battery, 0);
ZBUS_CHAN_ADD_OBS(TIME_CHAN, battery, 0);

#define MAX_MSG_SIZE \
	(MAX(sizeof(enum trigger_type), \
		(MAX(sizeof(struct network_msg), sizeof(enum time_status)))))

BUILD_ASSERT(CONFIG_APP_BATTERY_WATCHDOG_TIMEOUT_SECONDS >
			CONFIG_APP_BATTERY_EXEC_TIME_SECONDS_MAX,
			"Watchdog timeout must be greater than maximum execution time");

/* nPM1300 register bitmasks */

/* CHARGER.BCHGCHARGESTATUS.TRICKLECHARGE */
#define NPM1300_CHG_STATUS_TC_MASK BIT(2)
/* CHARGER.BCHGCHARGESTATUS.CONSTANTCURRENT */
#define NPM1300_CHG_STATUS_CC_MASK BIT(3)
/* CHARGER.BCHGCHARGESTATUS.CONSTANTVOLTAGE */
#define NPM1300_CHG_STATUS_CV_MASK BIT(4)

static const struct device *charger = DEVICE_DT_GET(DT_NODELABEL(npm1300_charger));

/* Forward declarations */
static struct s_object s_obj;
static int charger_read_sensors(float *voltage, float *current, float *temp, int32_t *chg_status);
static void sample(int64_t *ref_time);

/* State machine */

/* Defininig the module states.
 *
 * STATE_INIT: The battery module is initializing and waiting for time to be available.
 * STATE_SAMPLING: The battery module is ready to sample upon receiving a trigger.
 */
enum battery_module_state {
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

	/* Fuel gauge reference time */
	int64_t fuel_gauge_ref_time;
};

/* Forward declarations of state handlers */
static void state_init_entry(void *o);
static void state_init_run(void *o);
static void state_sampling_run(void *o);

static struct s_object battery_state_object;
static const struct smf_state states[] = {
	[STATE_INIT] =
		SMF_CREATE_STATE(state_init_entry, state_init_run, NULL,
				 NULL,	/* No parent state */
				 NULL), /* No initial transition */
	[STATE_SAMPLING] =
		SMF_CREATE_STATE(NULL, state_sampling_run, NULL,
				 NULL,
				 NULL),
};

/* State handlers */

static void state_init_entry(void *o)
{
	int err;
	struct sensor_value value;
	struct nrf_fuel_gauge_init_parameters parameters = {
		.model = &battery_model
	};
	int32_t chg_status;
	struct s_object *state_object = o;

	if (!device_is_ready(charger)) {
		LOG_ERR("Charger device not ready.");
		SEND_FATAL_ERROR();
		return;
	}

	err = charger_read_sensors(&parameters.v0, &parameters.i0, &parameters.t0, &chg_status);
	if (err < 0) {
		LOG_ERR("charger_read_sensors, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	err = nrf_fuel_gauge_init(&parameters, NULL);
	if (err) {
		LOG_ERR("nrf_fuel_gauge_init, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	state_object->fuel_gauge_ref_time = k_uptime_get();

	err = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_DESIRED_CHARGING_CURRENT, &value);
	if (err) {
		LOG_ERR("sensor_channel_get(DESIRED_CHARGING_CURRENT), error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

static void state_init_run(void *o)
{
	struct s_object *state_object = o;

	if (&TIME_CHAN == state_object->chan) {
		enum time_status time_status = MSG_TO_TIME_STATUS(state_object->msg_buf);

		if (time_status == TIME_AVAILABLE) {
			LOG_DBG("Time available, sampling can start");

			STATE_SET(battery_state_object, STATE_SAMPLING);
		}
	}
}

static void state_sampling_run(void *o)
{
	struct s_object *state_object = o;

	if (&TRIGGER_CHAN == state_object->chan) {
		enum trigger_type trigger_type = MSG_TO_TRIGGER_TYPE(state_object->msg_buf);

		if (trigger_type == TRIGGER_DATA_SAMPLE) {
			LOG_DBG("Data sample trigger received, getting battery data");
			sample(&state_object->fuel_gauge_ref_time);
		}
	}
}

/* End of state handling */

static int charger_read_sensors(float *voltage, float *current, float *temp, int32_t *chg_status)
{
	struct sensor_value value;
	int err;

	err = sensor_sample_fetch(charger);
	if (err < 0) {
		return err;
	}

	sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE, &value);
	*voltage = (float)value.val1 + ((float)value.val2 / 1000000);

	sensor_channel_get(charger, SENSOR_CHAN_GAUGE_TEMP, &value);
	*temp = (float)value.val1 + ((float)value.val2 / 1000000);

	sensor_channel_get(charger, SENSOR_CHAN_GAUGE_AVG_CURRENT, &value);
	*current = (float)value.val1 + ((float)value.val2 / 1000000);

	sensor_channel_get(charger, (enum sensor_channel)SENSOR_CHAN_NPM1300_CHARGER_STATUS,
			   &value);
	*chg_status = value.val1;

	return 0;
}

static void sample(int64_t *ref_time)
{
	int err;
	int chg_status;
	bool charging;
	float voltage;
	float current;
	float temp;
	float state_of_charge;
	float delta;
	int64_t system_time;

	err = date_time_now(&system_time);
	if (err) {
		LOG_ERR("Failed to convert uptime to unix time, error: %d", err);
		return;
	}

	err = charger_read_sensors(&voltage, &current, &temp, &chg_status);
	if (err) {
		LOG_ERR("charger_read_sensors, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	delta = (float)k_uptime_delta(ref_time) / 1000.f;

	charging = (chg_status & (NPM1300_CHG_STATUS_TC_MASK |
				  NPM1300_CHG_STATUS_CC_MASK |
				  NPM1300_CHG_STATUS_CV_MASK)) != 0;

	state_of_charge = nrf_fuel_gauge_process(voltage, current, temp, delta, false, NULL);

	LOG_DBG("State of charge: %f", (double)roundf(state_of_charge));
	LOG_DBG("The battery is %s", charging ? "charging" : "not charging");

	/* No further use of the battery data is implemented */
}

static void task_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void battery_task(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = (CONFIG_APP_BATTERY_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms = (CONFIG_APP_BATTERY_EXEC_TIME_SECONDS_MAX * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);

	LOG_DBG("Battery module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, task_wdt_callback, (void *)k_current_get());

	STATE_SET_INITIAL(battery_state_object, STATE_INIT);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = zbus_sub_wait_msg(&battery, &s_obj.chan, s_obj.msg_buf, zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = STATE_RUN(battery_state_object);
		if (err) {
			LOG_ERR("handle_message, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}
	}
}

K_THREAD_DEFINE(battery_task_id,
		CONFIG_APP_BATTERY_THREAD_STACK_SIZE,
		battery_task, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
