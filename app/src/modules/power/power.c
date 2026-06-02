/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/smf.h>
#include <date_time.h>
#include <zephyr/drivers/sensor/npm13xx_charger.h>
#include <nrf_fuel_gauge.h>
#include <math.h>
#include <modem/nrf_modem_lib.h>
#include "modem/lte_lc.h"

#include "lp803448_model.h"
#include "app_common.h"
#include "power.h"
#include "fuel_gauge_state.h"
#if defined(CONFIG_MEMFAULT_NRF_PLATFORM_BATTERY_NPM13XX)
#include "memfault/metrics/platform/battery.h"
#endif /* CONFIG_MEMFAULT_NRF_PLATFORM_BATTERY_NPM13XX */

LOG_MODULE_REGISTER(power, CONFIG_APP_POWER_LOG_LEVEL);

/* CHARGER.BCHGCHARGESTATUS.CHARGECOMPLETE */
#define NPM13XX_CHG_STATUS_COMPLETE_MASK BIT(1)
/* CHARGER.BCHGCHARGESTATUS.TRICKLECHARGE */
#define NPM13XX_CHG_STATUS_TC_MASK       BIT(2)
/* CHARGER.BCHGCHARGESTATUS.CONSTANTCURRENT */
#define NPM13XX_CHG_STATUS_CC_MASK       BIT(3)
/* CHARGER.BCHGCHARGESTATUS.CONSTANTVOLTAGE */
#define NPM13XX_CHG_STATUS_CV_MASK       BIT(4)
/* Active charging states mask */
#define NPM13XX_CHG_ACTIVE_MASK (NPM13XX_CHG_STATUS_TC_MASK | \
				 NPM13XX_CHG_STATUS_CC_MASK | \
				 NPM13XX_CHG_STATUS_CV_MASK)

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(power);

/* Define channels provided by this module */
ZBUS_CHAN_DEFINE(power_chan,
		 struct power_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Private channel message types for internal state management. */
enum priv_power_msg_type {
	/** Modem has been initialized */
	POWER_PRIV_MODEM_INITIALIZED,
	/** Modem is entering sleep mode */
	POWER_PRIV_MODEM_SLEEP_ENTRY,
	/** Modem is exiting sleep mode */
	POWER_PRIV_MODEM_SLEEP_EXIT,
	/** Timer for sampling data has expired. */
	POWER_PRIV_TIMER_EXPIRED,
};

struct priv_power_msg {
	/* Type of the message */
	enum priv_power_msg_type type;
};

/* Create private power channel for internal messaging that is not intended for external use. */
ZBUS_CHAN_DEFINE(priv_power_chan,
		 struct priv_power_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Define the channels that the module subscribes to, their associated message types
 * and the subscriber that will receive the messages on the channel.
 */
#define CHANNEL_LIST(X)                                                                            \
	X(power_chan, struct power_msg)                                                            \
	X(priv_power_chan, struct priv_power_msg)

/* Calculate the maximum message size from the list of channels */
#define MAX_MSG_SIZE MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

/* Add the power subscriber as observer to all the channels in the list. */
#define ADD_OBSERVERS(_chan, _type) ZBUS_CHAN_ADD_OBS(_chan, power, 0);

CHANNEL_LIST(ADD_OBSERVERS)

static void on_modem_init(int ret, void *ctx);

NRF_MODEM_LIB_ON_INIT(power_modem_init_hook, on_modem_init, NULL);

static void on_modem_init(int ret, void *ctx)
{
	int err;
	struct priv_power_msg msg = {.type = POWER_PRIV_MODEM_INITIALIZED};

	ARG_UNUSED(ctx);

	if (ret) {
		LOG_ERR("Modem init failed: %d", ret);
		return;
	}

	err = zbus_chan_pub(&priv_power_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

enum power_module_state {
	/* Waiting for modem initialization */
	STATE_WAITING_FOR_MODEM_INIT,
	/* The power module has started and is running */
	STATE_RUNNING,
	/* The device is idle, expected to be in low power state */
	STATE_IDLE,
	/* The device is active, performing tasks */
	STATE_ACTIVE,
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

	/* Pointer to the charger device */
	const struct device *const charger;

	/* Reference time for fuel gauge processing */
	int64_t fuel_gauge_ref_time;

	/* Battery percentage */
	float percentage;

	/* Battery voltage */
	float voltage;

	/* Battery current */
	float current;

	/* Battery temperature */
	float temperature;

	/* Charging status */
	bool charging;
};

/* Forward declarations of work function */
static void timer_sample_work_fn(struct k_work *work);

/* Delayable work used to schedule triggers */
K_WORK_DELAYABLE_DEFINE(timer_sample_work, timer_sample_work_fn);

/* Forward declarations of state handlers */
static enum smf_state_result state_waiting_for_modem_init_run(void *obj);
static void state_running_entry(void *obj);
static enum smf_state_result state_running_run(void *obj);
static void state_idle_entry(void *obj);
static enum smf_state_result state_idle_run(void *obj);
static void state_active_entry(void *obj);
static enum smf_state_result state_active_run(void *obj);
static void state_active_exit(void *obj);

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
				 &states[STATE_ACTIVE]),
	[STATE_IDLE] =
		SMF_CREATE_STATE(state_idle_entry,
				 state_idle_run,
				 NULL,
				 &states[STATE_RUNNING],
				 NULL),
	[STATE_ACTIVE] =
		SMF_CREATE_STATE(state_active_entry,
				 state_active_run,
				 state_active_exit,
				 &states[STATE_RUNNING],
				 NULL),
};

static void power_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s", channel_id,
		k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void timer_sample_work_fn(struct k_work *work)
{
	int err;
	const struct priv_power_msg msg = {.type = POWER_PRIV_TIMER_EXPIRED};

	ARG_UNUSED(work);

	err = zbus_chan_pub(&priv_power_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("Failed to publish, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static void timer_sample_start(uint32_t delay_ms)
{
	int err;

	err = k_work_reschedule(&timer_sample_work, K_MSEC(delay_ms));
	if (err < 0) {
		LOG_ERR("reschedule timer_sample_work, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static void timer_sample_stop(void)
{
	int err;

	err = k_work_cancel_delayable(&timer_sample_work);
	if (err < 0) {
		LOG_ERR("cancel timer_sample_work, error: %d", err);
	}
}

static void send_battery_percentage_sample_response(const struct power_state_object *state_object)
{
	int err;
	struct power_msg response = {
		.type = POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE,
	};

	response.percentage = roundf(state_object->percentage);
	response.charging = state_object->charging;
	response.voltage = state_object->voltage;
	err = date_time_now(&response.timestamp);
	if (err != 0 && err != -ENODATA) {
		LOG_ERR("date_time_now, error: %d", err);
	}

	err = zbus_chan_pub(&power_chan, &response, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub sample response, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

#if defined(CONFIG_APP_POWER_SHELL)
static void log_battery_sample(const struct power_state_object *state_object)
{
	LOG_INF("Battery percentage: %.2f%%", (double)state_object->percentage);
	LOG_INF("Battery voltage: %.4f V", (double)state_object->voltage);
	LOG_INF("Battery current: %.4f A", (double)state_object->current);
	LOG_INF("Battery temperature: %.2f °C", (double)state_object->temperature);
	LOG_INF("Battery is %s charging", state_object->charging ? "" : "not ");
}
#endif /* CONFIG_APP_POWER_SHELL */

static int charger_read_sensors(const struct device *charger, float *voltage, float *current,
				float *temp, int32_t *chg_status, bool *vbus_connected)
{
	int err;
	struct sensor_value value = {0};

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

	/* Zephyr sensor API returns current as negative for discharging, positive for charging,
	 * but nRF fuel gauge library expects opposite. Flip here for uniformity.
	 */
	*current = -sensor_value_to_float(&value);

	err = sensor_channel_get(charger, (enum sensor_channel)SENSOR_CHAN_NPM13XX_CHARGER_STATUS,
				 &value);
	if (err) {
		return err;
	}

	*chg_status = value.val1;

	if (vbus_connected != NULL) {
		struct sensor_value vbus_present;

		err = sensor_attr_get(
			charger, (enum sensor_channel)SENSOR_CHAN_NPM13XX_CHARGER_VBUS_STATUS,
			(enum sensor_attribute)SENSOR_ATTR_NPM13XX_CHARGER_VBUS_PRESENT,
			&vbus_present);
		if (err == 0) {
			*vbus_connected = (vbus_present.val1 != 0);
		} else {
			LOG_DBG("Failed to read VBUS state: %d", err);
			*vbus_connected = false;
		}
	}

	return 0;
}

static bool power_is_charging(int32_t chg_status)
{
	return (chg_status & NPM13XX_CHG_ACTIVE_MASK) != 0;
}

static void power_update_charge_state_if_changed(int32_t chg_status, int32_t *prev_chg_status)
{
	union nrf_fuel_gauge_ext_state_info_data ext_data;

	if (chg_status == *prev_chg_status) {
		return;
	}

	*prev_chg_status = chg_status;

	if (chg_status & NPM13XX_CHG_STATUS_COMPLETE_MASK) {
		ext_data.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_COMPLETE;
	} else if (chg_status & NPM13XX_CHG_STATUS_TC_MASK) {
		ext_data.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_TRICKLE;
	} else if (chg_status & NPM13XX_CHG_STATUS_CC_MASK) {
		ext_data.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_CC;
	} else if (chg_status & NPM13XX_CHG_STATUS_CV_MASK) {
		ext_data.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_CV;
	} else {
		ext_data.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_IDLE;
	}

	(void)nrf_fuel_gauge_ext_state_update(NRF_FUEL_GAUGE_EXT_STATE_INFO_CHARGE_STATE_CHANGE,
					      &ext_data);
}

static int sample_and_process(struct power_state_object *state_object)
{
	int err;
	static int32_t prev_chg_status = -1;
	int32_t chg_status;
	bool vbus_connected;
	float delta;
#if defined(CONFIG_MEMFAULT_NRF_PLATFORM_BATTERY_NPM13XX)
	sMfltPlatformBatterySoc soc;
#endif /* CONFIG_MEMFAULT_NRF_PLATFORM_BATTERY_NPM13XX */

	err = charger_read_sensors(state_object->charger, &state_object->voltage,
				   &state_object->current, &state_object->temperature, &chg_status,
				   &vbus_connected);
	if (err) {
		LOG_ERR("Failed to read charger sensors: %d", err);
		return err;
	}

	state_object->charging = power_is_charging(chg_status);

	/* Inform fuel gauge of VBUS state */
	(void)nrf_fuel_gauge_ext_state_update(
		vbus_connected ? NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_CONNECTED
			       : NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_DISCONNECTED,
		NULL);

	/* Inform fuel gauge of charge state changes */
	power_update_charge_state_if_changed(chg_status, &prev_chg_status);

#if defined(CONFIG_MEMFAULT_NRF_PLATFORM_BATTERY_NPM13XX)
	err = memfault_platform_get_stateofcharge(&soc);
	if (err) {
		LOG_ERR("memfault_platform_get_stateofcharge, error: %d", err);
		return err;
	}

	state_object->percentage =
		(float)soc.soc / (float)CONFIG_MEMFAULT_METRICS_BATTERY_SOC_PCT_SCALE_VALUE;

	(void)delta;
#else

	delta = (float)k_uptime_delta(&state_object->fuel_gauge_ref_time) / 1000.0f;

	err = nrf_fuel_gauge_process(state_object->voltage,
				     state_object->current,
				     state_object->temperature, delta,
				     &state_object->percentage, NULL);
	if (err) {
		LOG_ERR("nrf_fuel_gauge_process, error: %d", err);
		return err;
	}

#endif /* CONFIG_MEMFAULT_NRF_PLATFORM_BATTERY_NPM13XX */

	err = fuel_gauge_state_save();
	if (err) {
		LOG_WRN("Failed to save fuel gauge state: %d", err);
	}

	timer_sample_start(CONFIG_APP_POWER_SAMPLE_INTERVAL_MS);

	return 0;
}

static void lte_lc_evt_handler(const struct lte_lc_evt *const evt)
{

	switch (evt->type) {
	case LTE_LC_EVT_MODEM_SLEEP_EXIT:

		struct priv_power_msg msg = {
			.type = POWER_PRIV_MODEM_SLEEP_EXIT,
		};
		int err = zbus_chan_pub(&priv_power_chan, &msg, PUB_TIMEOUT);
		if (err) {
			LOG_ERR("Failed to publish modem sleep exit message, error: %d", err);
			SEND_FATAL_ERROR();
		}

		break;
	case LTE_LC_EVT_MODEM_SLEEP_ENTER:

		struct priv_power_msg msg_enter = {
			.type = POWER_PRIV_MODEM_SLEEP_ENTRY,
		};
		err = zbus_chan_pub(&priv_power_chan, &msg_enter, PUB_TIMEOUT);
		if (err) {
			LOG_ERR("Failed to publish modem sleep entry message, error: %d", err);
			SEND_FATAL_ERROR();
		}
		break;
	default:
		break;
	}
}

static enum smf_state_result state_waiting_for_modem_init_run(void *obj)
{
	const struct power_state_object *state_object = obj;

	if (state_object->chan == &priv_power_chan) {
		const struct priv_power_msg *msg =
			(const struct priv_power_msg *)state_object->msg_buf;

		if (msg->type == POWER_PRIV_MODEM_INITIALIZED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_RUNNING]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_running_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	int err;
	const struct power_msg msg = {.type = POWER_MODULE_READY};

	lte_lc_register_handler(lte_lc_evt_handler);

	err = zbus_chan_pub(&power_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

static enum smf_state_result state_running_run(void *obj)
{
	const struct power_state_object *state_object = obj;

	/* Handle sample requests at top level */
	if (state_object->chan == &power_chan) {
		const struct power_msg *power_msg = (const struct power_msg *)state_object->msg_buf;

		if (power_msg->type == POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST) {
			send_battery_percentage_sample_response(state_object);

			return SMF_EVENT_HANDLED;
		}
#if defined(CONFIG_APP_POWER_SHELL)
		if (power_msg->type == POWER_BATTERY_SAMPLE_LOG) {
			log_battery_sample(state_object);
			return SMF_EVENT_HANDLED;
		}
#endif /* CONFIG_APP_POWER_SHELL */
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_idle_entry(void *obj)
{
	const struct power_state_object *state_object = obj;

	float idle_current = ((float)CONFIG_APP_POWER_IDLE_CURRENT_NA / 1e9f);

	LOG_DBG("%s", __func__);

	nrf_fuel_gauge_idle_set(state_object->voltage, state_object->temperature, idle_current);
}

static enum smf_state_result state_idle_run(void *obj)
{
	const struct power_state_object *state_object = obj;

	if (state_object->chan == &priv_power_chan) {
		const struct priv_power_msg *msg =
			(const struct priv_power_msg *)state_object->msg_buf;
		if (msg->type == POWER_PRIV_MODEM_SLEEP_EXIT) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_ACTIVE]);

			return SMF_EVENT_HANDLED;
		}

		if (msg->type == POWER_PRIV_MODEM_SLEEP_ENTRY) {
			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_active_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	/* Start periodic sampling timer */
	timer_sample_start(CONFIG_APP_POWER_SAMPLE_INTERVAL_MS);
}

static enum smf_state_result state_active_run(void *obj)
{
	struct power_state_object *state_object = obj;

	if (state_object->chan == &priv_power_chan) {
		const struct priv_power_msg *msg =
			(const struct priv_power_msg *)state_object->msg_buf;

		if (msg->type == POWER_PRIV_TIMER_EXPIRED) {
			sample_and_process(state_object);
			return SMF_EVENT_HANDLED;
		}

		if (msg->type == POWER_PRIV_MODEM_SLEEP_ENTRY) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_IDLE]);
			return SMF_EVENT_HANDLED;
		}

		if (msg->type == POWER_PRIV_MODEM_SLEEP_EXIT) {
			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_active_exit(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	/* Stop periodic sampling timer */
	timer_sample_stop();
}

static void power_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = (CONFIG_APP_POWER_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_POWER_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	static struct power_state_object power_state = {
		.charger = DEVICE_DT_GET(DT_NODELABEL(npm1300_charger)),
	};
	int32_t chg_status;
	struct nrf_fuel_gauge_init_parameters parameters = {.model = &battery_model};

	LOG_DBG("Power module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, power_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		SEND_FATAL_ERROR();
		return;
	}

	if (!device_is_ready(power_state.charger)) {
		LOG_ERR("Charger device not ready");
		SEND_FATAL_ERROR();
		return;
	}

	err = charger_read_sensors(power_state.charger, &parameters.v0, &parameters.i0,
				   &parameters.t0, &chg_status, NULL);
	if (err) {
		LOG_ERR("charger_read_sensors, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	parameters.state = fuel_gauge_state_get();
	if (parameters.state) {
		LOG_DBG("Restoring fuel gauge from saved state (%zu bytes)",
			fuel_gauge_state_size_get());
	} else {
		LOG_DBG("No saved fuel gauge state found, initializing from scratch");
	}

	err = nrf_fuel_gauge_init(&parameters, NULL);
	if (err) {
		LOG_ERR("nrf_fuel_gauge_init, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	/* Seed the initial SoC */
	power_state.voltage = parameters.v0;
	power_state.current = parameters.i0;
	power_state.temperature = parameters.t0;

	err = nrf_fuel_gauge_process(parameters.v0,
				     parameters.i0,
				     parameters.t0,
				     0.0f,
				     &power_state.percentage,
				     NULL);
	if (err) {
		LOG_ERR("nrf_fuel_gauge_process, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	/* Set charge current limit and termination current for accurate TTF prediction */
	struct sensor_value desired_charge_current;

	err = sensor_channel_get(power_state.charger, SENSOR_CHAN_GAUGE_DESIRED_CHARGING_CURRENT,
				 &desired_charge_current);
	if (err == 0) {
		float charge_current_limit = sensor_value_to_float(&desired_charge_current);
		union nrf_fuel_gauge_ext_state_info_data ext_data = {
			.charge_current_limit = charge_current_limit,
		};

		(void)nrf_fuel_gauge_ext_state_update(
			NRF_FUEL_GAUGE_EXT_STATE_INFO_CHARGE_CURRENT_LIMIT, &ext_data);

		ext_data.charge_term_current = charge_current_limit / 10.0f;
		(void)nrf_fuel_gauge_ext_state_update(NRF_FUEL_GAUGE_EXT_STATE_INFO_TERM_CURRENT,
						      &ext_data);
	} else {
		LOG_DBG("Failed to read desired charge current: %d", err);
	}

	power_state.fuel_gauge_ref_time = k_uptime_get();

	/* Initialize the state machine */
	smf_set_initial(SMF_CTX(&power_state), &states[STATE_WAITING_FOR_MODEM_INIT]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("Failed to feed the watchdog: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = zbus_sub_wait_msg(&power, &power_state.chan, power_state.msg_buf,
					zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait, error: %d", err);
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

K_THREAD_DEFINE(power_module_thread_id, CONFIG_APP_POWER_THREAD_STACK_SIZE, power_module_thread,
		NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
