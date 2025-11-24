/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/pm/device.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/drivers/sensor/npm13xx_charger.h>
#include <zephyr/drivers/mfd/npm13xx.h>
#include <zephyr/sys/util.h>
#include <nrf_fuel_gauge.h>
#include <date_time.h>
#include <math.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/smf.h>
#include <modem/nrf_modem_lib_trace.h>

#include "lp803448_model.h"
#include "app_common.h"
#include "power.h"

/* Register log module */
LOG_MODULE_REGISTER(power, CONFIG_APP_POWER_LOG_LEVEL);

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(power);

/* Define channels provided by this module */
ZBUS_CHAN_DEFINE(POWER_CHAN,
		 struct power_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Observe channels */
ZBUS_CHAN_ADD_OBS(POWER_CHAN, power, 0);

#define MAX_MSG_SIZE sizeof(struct power_msg)

BUILD_ASSERT(CONFIG_APP_POWER_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_POWER_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

/* nPM13xx register bitmasks */

/* CHARGER.BCHGCHARGESTATUS.TRICKLECHARGE */
#define NPM13XX_CHG_STATUS_TC_MASK BIT(2)
/* CHARGER.BCHGCHARGESTATUS.CONSTANTCURRENT */
#define NPM13XX_CHG_STATUS_CC_MASK BIT(3)
/* CHARGER.BCHGCHARGESTATUS.CONSTANTVOLTAGE */
#define NPM13XX_CHG_STATUS_CV_MASK BIT(4)

static const struct device *charger = DEVICE_DT_GET(DT_NODELABEL(npm1300_charger));
static const struct device *pmic = DEVICE_DT_GET(DT_NODELABEL(pmic_main));

static const struct device *const uart0_dev = DEVICE_DT_GET(DT_NODELABEL(uart0));
static const struct device *const uart1_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));

/* Forward declarations */
static int subscribe_to_vsbus_events(const struct device *pmic, struct gpio_callback *event_cb);
static int charger_read_sensors(float *voltage, float *current, float *temp, int32_t *chg_status);
static void sample(int64_t *ref_time);

/* State machine */

/* Power module states.
 */
enum power_module_state {
	STATE_RUNNING,
};

/* State object.
 * Used to transfer context data between state changes.
 */
struct power_state_object {
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
static void state_running_entry(void *obj);
static enum smf_state_result state_running_run(void *obj);

/* State machine definition */
static const struct smf_state states[] = {
	[STATE_RUNNING] =
		SMF_CREATE_STATE(state_running_entry, state_running_run, NULL, NULL, NULL),
};


/* State handlers */

static void state_running_entry(void *obj)
{
	int err;
	struct sensor_value value;
	struct nrf_fuel_gauge_init_parameters parameters = {
		.model = &battery_model
	};
	int32_t chg_status;
	struct power_state_object *state_object = obj;
	static struct gpio_callback event_cb;

	if (!device_is_ready(charger)) {
		LOG_ERR("Charger device not ready.");
		SEND_FATAL_ERROR();
		return;
	}

	err = subscribe_to_vsbus_events(pmic, &event_cb);
	if (err) {
		LOG_ERR("subscribe_to_vsbus_events, error: %d", err);
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
		LOG_ERR("sensor_channel_get, DESIRED_CHARGING_CURRENT, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

static enum smf_state_result state_running_run(void *obj)
{
	struct power_state_object *state_object = obj;

	if (&POWER_CHAN == state_object->chan) {
		struct power_msg msg = MSG_TO_POWER_MSG(state_object->msg_buf);

		if (msg.type == POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST) {
			LOG_DBG("Battery percentage sample request received, getting battery data");
			sample(&state_object->fuel_gauge_ref_time);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static int uart_disable(void)
{
	int err;

	if (!device_is_ready(uart0_dev) || !device_is_ready(uart1_dev)) {
		LOG_ERR("UART devices are not ready");
		return -ENODEV;
	}

#ifdef CONFIG_NRF_MODEM_LIB_TRACE_BACKEND_UART
	err = nrf_modem_lib_trace_level_set(NRF_MODEM_LIB_TRACE_LEVEL_OFF);
	if (err) {
		LOG_ERR("nrf_modem_lib_trace_level_set, error: %d", err);
		return err;
	}
#endif

	/* Wait for UART buffers to be emptied before suspending.
	 * If a transfer is ongoing, the driver will cause an assertion to fail.
	 * 100 ms is an arbitrary value that should be enough for the buffers to empty.
	 */
	k_busy_wait(100 * USEC_PER_MSEC);

	err = pm_device_action_run(uart1_dev, PM_DEVICE_ACTION_SUSPEND);
	if (err && (err != -EALREADY)) {
		LOG_ERR("pm_device_action_run, error: %d", err);
		return err;
	}

	err = pm_device_action_run(uart0_dev, PM_DEVICE_ACTION_SUSPEND);
	if (err && (err != -EALREADY)) {
		LOG_ERR("pm_device_action_run, error: %d", err);
		return err;
	}

	LOG_DBG("UART devices disabled");
	return 0;
}

static int uart_enable(void)
{
	int err;

	if (!device_is_ready(uart0_dev) || !device_is_ready(uart1_dev)) {
		LOG_ERR("UART devices are not ready");
		return -ENODEV;
	}

#ifdef CONFIG_NRF_MODEM_LIB_TRACE_BACKEND_UART
	err = nrf_modem_lib_trace_level_set(CONFIG_NRF_MODEM_LIB_TRACE_LEVEL_FULL);
	if (err) {
		LOG_ERR("nrf_modem_lib_trace_level_set, error: %d", err);
		return err;
	}
#endif

	err = pm_device_action_run(uart0_dev, PM_DEVICE_ACTION_RESUME);
	if (err && (err != -EALREADY)) {
		LOG_ERR("pm_device_action_run, error: %d", err);
		return err;
	}

	err = pm_device_action_run(uart1_dev, PM_DEVICE_ACTION_RESUME);
	if (err && (err != -EALREADY)) {
		LOG_ERR("pm_device_action_run, error: %d", err);
		return err;
	}

	LOG_DBG("UART devices enabled");
	return 0;
}

static void event_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);

	int err;

	if (pins & BIT(NPM13XX_EVENT_VBUS_DETECTED)) {
		LOG_DBG("VBUS detected");

		if (IS_ENABLED(CONFIG_APP_POWER_DISABLE_UART_ON_VBUS_REMOVED)) {
			err = uart_enable();
			if (err) {
				LOG_ERR("uart_enable, error: %d", err);
				SEND_FATAL_ERROR();
				return;
			}
		}
	}

	if (pins & BIT(NPM13XX_EVENT_VBUS_REMOVED)) {
		LOG_DBG("VBUS removed");

		if (IS_ENABLED(CONFIG_APP_POWER_DISABLE_UART_ON_VBUS_REMOVED)) {
			err = uart_disable();
			if (err) {
				LOG_ERR("uart_disable, error: %d", err);
				SEND_FATAL_ERROR();
				return;
			}
		}
	}
}

static int subscribe_to_vsbus_events(const struct device *device, struct gpio_callback *event_cb)
{
	int err;

	gpio_init_callback(event_cb, event_callback, BIT(NPM13XX_EVENT_VBUS_DETECTED) |
						     BIT(NPM13XX_EVENT_VBUS_REMOVED));

	err = mfd_npm13xx_add_callback(device, event_cb);
	if (err) {
		LOG_ERR("mfd_npm13xx_add_callback, error: %d", err);
		return err;
	}

	return 0;
}

static int charger_read_sensors(float *voltage, float *current, float *temp, int32_t *chg_status)
{
	struct sensor_value value = {0};
	int err;

	err = sensor_sample_fetch(charger);
	if (err < 0) {
		return err;
	}

	err = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE, &value);
	if (err) {
		return err;
	}

	*voltage = sensor_value_to_float(&value);

	err = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_TEMP, &value);
	if (err) {
		return err;
	}

	*temp = sensor_value_to_float(&value);

	err = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_AVG_CURRENT, &value);
	if (err) {
		return err;
	}

	/* Zephyr sensor API returns current as negative for discharging, positive for charging
	 * but nRF fuel gauge library expects opposite. Flip here for uniformity
	 */
	*current = -sensor_value_to_float(&value);

	err = sensor_channel_get(charger, (enum sensor_channel)SENSOR_CHAN_NPM13XX_CHARGER_STATUS,
			   &value);
	if (err) {
		return err;
	}

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

	err = charger_read_sensors(&voltage, &current, &temp, &chg_status);
	if (err) {
		LOG_ERR("charger_read_sensors, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	delta = (float)k_uptime_delta(ref_time) / 1000.f;

	charging = (chg_status & (NPM13XX_CHG_STATUS_TC_MASK |
				  NPM13XX_CHG_STATUS_CC_MASK |
				  NPM13XX_CHG_STATUS_CV_MASK)) != 0;

	state_of_charge = nrf_fuel_gauge_process(voltage, current, temp, delta, NULL);

	LOG_DBG("State of charge: %f", (double)roundf(state_of_charge));
	LOG_DBG("The battery is %s", charging ? "charging" : "not charging");
	LOG_DBG("Battery voltage: %f V", (double)voltage);
	LOG_DBG("Battery current: %f A", (double)current);
	LOG_DBG("Battery temperature: %f C", (double)temp);

	struct power_msg msg = {
		.type = POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE,
		.percentage = (double)roundf(state_of_charge),
		.charging = charging,
		.voltage = (double)voltage,
	};

#if defined(CONFIG_APP_POWER_TIMESTAMP)
	err = date_time_now(&msg.timestamp);
	if (err) {
		LOG_ERR("date_time_now() failed, error: %d, using 0", err);
	}
#endif /* CONFIG_APP_POWER_TIMESTAMP */

	err = zbus_chan_pub(&POWER_CHAN, &msg, K_NO_WAIT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

static void power_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void power_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms =
		(CONFIG_APP_POWER_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_POWER_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	struct power_state_object power_state;

	LOG_DBG("Power module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, power_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		SEND_FATAL_ERROR();
		return;
	}

	smf_set_initial(SMF_CTX(&power_state), &states[STATE_RUNNING]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = zbus_sub_wait_msg(&power,
					&power_state.chan,
					power_state.msg_buf,
					zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = smf_run_state(SMF_CTX(&power_state));
		if (err) {
			LOG_ERR("smf_run_state(), error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}
	}
}

K_THREAD_DEFINE(power_module_thread_id,
		CONFIG_APP_POWER_THREAD_STACK_SIZE,
		power_module_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
