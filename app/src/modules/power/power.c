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
#include <math.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/smf.h>
#include <date_time.h>
#include <modem/nrf_modem_lib_trace.h>
#include <modem/nrf_modem_lib.h>

#include "lp803448_model.h"
#include "app_common.h"
#include "power.h"

/* Register log module */
LOG_MODULE_REGISTER(power, CONFIG_APP_POWER_LOG_LEVEL);

/* Battery drain detection configuration */
#define BATTERY_HISTORY_SIZE 20  /* Store up to 20 readings */
#define BATTERY_DRAIN_THRESHOLD_PCT 5.0  /* 5% drop threshold */
#define BATTERY_DRAIN_WINDOW_MS (60 * 60 * 1000)  /* 1 hour in milliseconds */

/* Battery reading structure for historical tracking */
struct battery_reading {
	int64_t timestamp;  /* Unix time in milliseconds */
	double percentage;  /* Battery percentage */
};

/* Circular buffer for battery history */
static struct battery_reading battery_history[BATTERY_HISTORY_SIZE];
static uint8_t battery_history_index = 0;
static uint8_t battery_history_count = 0;
static bool previous_charging_state = false;

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

/* Private channel message types for internal state management. */
enum priv_power_msg {
	/* Modem has completed initialization. */
	POWER_PRIV_MODEM_INITIALIZED,
};

/* Create private power channel for internal messaging that is not intended for external use. */
ZBUS_CHAN_DEFINE(PRIV_POWER_CHAN,
		 enum priv_power_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(power),
		 ZBUS_MSG_INIT(0)
);

/* Define the channels that the module subscribes to, their associated message types
 * and the subscriber that will receive the messages on the channel.
 */
#define CHANNEL_LIST(X)							\
	X(POWER_CHAN,		struct power_msg)			\
	X(PRIV_POWER_CHAN,	enum priv_power_msg)			\

/* Calculate the maximum message size from the list of channels */
#define MAX_MSG_SIZE			MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

/* Add the power subscriber as observer to all the channels in the list. */
#define ADD_OBSERVERS(_chan, _type)	ZBUS_CHAN_ADD_OBS(_chan, power, 0);

/*
 * Expand to a call to ZBUS_CHAN_ADD_OBS for each channel in the list.
 * Example: ZBUS_CHAN_ADD_OBS(POWER_CHAN, power, 0);
 */
CHANNEL_LIST(ADD_OBSERVERS)

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
static int vbus_is_present(void);
static int sync_uart_to_vbus_status(void);
static void on_modem_init(int ret, void *ctx);
static void add_battery_reading(double percentage, int64_t timestamp);
static void check_battery_drain(double current_percentage, int64_t current_timestamp);
static void clear_battery_history(void);

NRF_MODEM_LIB_ON_INIT(power_modem_init_hook, on_modem_init, NULL);

static void on_modem_init(int ret, void *ctx)
{
	int err;
	enum priv_power_msg msg = POWER_PRIV_MODEM_INITIALIZED;

	ARG_UNUSED(ctx);

	if (ret) {
		LOG_ERR("Modem init failed: %d, power module cannot initialize", ret);
		return;
	}

	err = zbus_chan_pub(&PRIV_POWER_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

/* State machine */

/* Power module states.
 */
enum power_module_state {
	/* Waiting for modem initialization */
	STATE_WAITING_FOR_MODEM_INIT,
	/* The module is running */
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

	/* Last received message */
	uint8_t msg_buf[MAX_MSG_SIZE];

	/* Fuel gauge reference time */
	int64_t fuel_gauge_ref_time;
};

/* Forward declarations of state handlers */
static enum smf_state_result state_waiting_for_modem_init_run(void *obj);
static void state_running_entry(void *obj);
static enum smf_state_result state_running_run(void *obj);

/* Construct state table */
static const struct smf_state states[] = {
	[STATE_WAITING_FOR_MODEM_INIT] =
		SMF_CREATE_STATE(NULL,
				 state_waiting_for_modem_init_run,
				 NULL,
				 NULL,
				 NULL),
	[STATE_RUNNING] =
		SMF_CREATE_STATE(state_running_entry,
				 state_running_run,
				 NULL,
				 NULL,
				 NULL),
};

/* State handlers */

static enum smf_state_result state_waiting_for_modem_init_run(void *obj)
{
	struct power_state_object *state_object = obj;

	if (state_object->chan == &PRIV_POWER_CHAN) {
		enum priv_power_msg msg = *(const enum priv_power_msg *)state_object->msg_buf;

		if (msg == POWER_PRIV_MODEM_INITIALIZED) {
			LOG_DBG("Modem initialized, transitioning to running state");
			smf_set_state(SMF_CTX(state_object), &states[STATE_RUNNING]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

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

	err = sync_uart_to_vbus_status();
	if (err) {
		LOG_ERR("sync_uart_to_vbus_status, error: %d", err);
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
	err = nrf_modem_lib_trace_level_set(CONFIG_NRF_MODEM_LIB_TRACE_LEVEL);
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

		/* Clear battery history when USB power is connected */
		clear_battery_history();
		LOG_WRN("VBUS connected, clearing battery history");

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

static int vbus_is_present(void)
{
	int err;
	struct sensor_value value;

	err = sensor_attr_get(charger,
			      (enum sensor_channel)SENSOR_CHAN_NPM13XX_CHARGER_VBUS_STATUS,
			      (enum sensor_attribute)SENSOR_ATTR_NPM13XX_CHARGER_VBUS_PRESENT,
			      &value);
	if (err) {
		LOG_ERR("sensor_attr_get, error: %d", err);
		return err;
	}

	return value.val1 != 0;
}

static int sync_uart_to_vbus_status(void)
{
	int present;

	if (!IS_ENABLED(CONFIG_APP_POWER_DISABLE_UART_ON_VBUS_REMOVED)) {
		return 0;
	}

	present = vbus_is_present();
	if (present < 0) {
		return present;
	}

	return present ? uart_enable() : uart_disable();
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

#if defined(CONFIG_MEMFAULT_NRF_PLATFORM_BATTERY_NPM13XX)
#include "memfault/metrics/platform/battery.h"
#endif /* CONFIG_MEMFAULT_NRF_PLATFORM_BATTERY_NPM13XX */

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
#if defined(CONFIG_MEMFAULT_NRF_PLATFORM_BATTERY_NPM13XX)
	sMfltPlatformBatterySoc soc;
#endif /* CONFIG_MEMFAULT_NRF_PLATFORM_BATTERY_NPM13XX */

	err = charger_read_sensors(&voltage, &current, &temp, &chg_status);
	if (err) {
		LOG_ERR("charger_read_sensors, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

#if defined(CONFIG_MEMFAULT_NRF_PLATFORM_BATTERY_NPM13XX)

	err = memfault_platform_get_stateofcharge(&soc);
	if (err) {
		LOG_ERR("memfault_platform_get_stateofcharge, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	state_of_charge = (float)soc.soc / (float)CONFIG_MEMFAULT_METRICS_BATTERY_SOC_PCT_SCALE_VALUE;
	charging = !soc.discharging;

	(void)delta;

#else /* CONFIG_MEMFAULT_NRF_PLATFORM_BATTERY_NPM13XX */

	delta = (float)k_uptime_delta(ref_time) / 1000.f;

	charging = (chg_status & (NPM13XX_CHG_STATUS_TC_MASK |
				  NPM13XX_CHG_STATUS_CC_MASK |
				  NPM13XX_CHG_STATUS_CV_MASK)) != 0;

	state_of_charge = nrf_fuel_gauge_process(voltage, current, temp, delta, NULL);
#endif /* CONFIG_MEMFAULT_NRF_PLATFORM_BATTERY_NPM13XX */

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
		.timestamp = k_uptime_get()
	};

	err = date_time_now(&msg.timestamp);
	if (err != 0 && err != -ENODATA) {
		LOG_ERR("date_time_now, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	err = zbus_chan_pub(&POWER_CHAN, &msg, K_NO_WAIT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	/* Detect charging state change and clear history if charging just started */
	if (charging && !previous_charging_state) {
		LOG_INF("Charging started, clearing battery history");
		clear_battery_history();
	}
	previous_charging_state = charging;

	/* Track battery drain when not charging */
	if (!charging) {
		add_battery_reading(msg.percentage, msg.timestamp);
		check_battery_drain(msg.percentage, msg.timestamp);
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
	static struct power_state_object power_state;

	LOG_DBG("Power module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, power_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		SEND_FATAL_ERROR();
		return;
	}

	/* Initialize the state machine */
	smf_set_initial(SMF_CTX(&power_state), &states[STATE_WAITING_FOR_MODEM_INIT]);

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

/* Clear all battery history */
static void clear_battery_history(void)
{
	battery_history_index = 0;
	battery_history_count = 0;
	LOG_DBG("Battery history cleared");
}

/* Add a new battery reading to the circular buffer */
static void add_battery_reading(double percentage, int64_t timestamp)
{
	/* Store reading in circular buffer */
	battery_history[battery_history_index].percentage = percentage;
	battery_history[battery_history_index].timestamp = timestamp;
	
	/* Update index and count */
	battery_history_index = (battery_history_index + 1) % BATTERY_HISTORY_SIZE;
	if (battery_history_count < BATTERY_HISTORY_SIZE) {
		battery_history_count++;
	}
	
	LOG_WRN("Stored battery reading: %.1f%% at timestamp: %lld",
		percentage, timestamp);
}

/* Check if battery has drained more than threshold in the last hour */
static void check_battery_drain(double current_percentage, int64_t current_timestamp)
{
	if (battery_history_count < 2) {
		/* Not enough data to check drain */
		LOG_DBG("Insufficient data for drain check (count: %d)", battery_history_count);
		return;
	}
	
	/* Find the oldest reading within the 1-hour window */
	double max_drop = 0.0;
	int64_t oldest_timestamp_in_window = 0;
	double oldest_percentage_in_window = current_percentage;
	
	for (int i = 0; i < battery_history_count; i++) {
		int64_t timestamp_diff = current_timestamp - battery_history[i].timestamp;
		
		/* Check if this reading is within the 1-hour window */
		if (timestamp_diff <= BATTERY_DRAIN_WINDOW_MS && timestamp_diff > 0) {
			double percentage_drop = battery_history[i].percentage - current_percentage;
			
			if (percentage_drop > max_drop) {
				max_drop = percentage_drop;
				oldest_timestamp_in_window = battery_history[i].timestamp;
				oldest_percentage_in_window = battery_history[i].percentage;
			}
			
			LOG_WRN("Checking reading from %lld ms ago: %.1f%% (drop: %.1f%%)",
				timestamp_diff, battery_history[i].percentage, percentage_drop);
		}
	}
	
	if (max_drop > BATTERY_DRAIN_THRESHOLD_PCT) {
		int64_t time_window_sec = (current_timestamp - oldest_timestamp_in_window) / 1000;
		
		LOG_ERR("ABNORMAL BATTERY DRAIN DETECTED!");
		LOG_ERR("Battery dropped %.1f%% in %lld seconds (%.1f minutes)",
			max_drop, time_window_sec, (double)time_window_sec / 60.0);
		LOG_ERR("From %.1f%% to %.1f%%", 
			oldest_percentage_in_window, current_percentage);
		
		/* Assert to indicate abnormal condition */
		SEND_FATAL_ERROR();
	} else {
		LOG_WRN("Battery drain within normal range: %.1f%% drop in last hour", max_drop);
	}
}
