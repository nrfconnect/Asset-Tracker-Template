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

#include "modules_common.h"
#include "message_channel.h"
#include "button.h"
#include "network.h"
#include "cloud_module.h"
#include "fota.h"
#include "location.h"

#if defined(CONFIG_APP_LED)
#include "led.h"
#endif /* CONFIG_APP_LED */

#if defined(CONFIG_APP_ENVIRONMENTAL)
#include "environmental.h"
#endif /* CONFIG_APP_ENVIRONMENTAL */

#if defined(CONFIG_APP_BATTERY)
#include "power.h"
#endif /* CONFIG_APP_BATTERY */

/* Register log module */
LOG_MODULE_REGISTER(app, CONFIG_APP_LOG_LEVEL);

/* Define a ZBUS listener for this module */
static void app_callback(const struct zbus_channel *chan);
ZBUS_LISTENER_DEFINE(app_listener, app_callback);

ZBUS_CHAN_DEFINE(TIMER_CHAN,
		 int,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Observe channels */
ZBUS_CHAN_ADD_OBS(CONFIG_CHAN, app_listener, 0);
ZBUS_CHAN_ADD_OBS(CLOUD_CHAN, app_listener, 0);
ZBUS_CHAN_ADD_OBS(BUTTON_CHAN, app_listener, 0);
ZBUS_CHAN_ADD_OBS(FOTA_CHAN, app_listener, 0);
ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, app_listener, 0);
ZBUS_CHAN_ADD_OBS(LOCATION_CHAN, app_listener, 0);
ZBUS_CHAN_ADD_OBS(TIMER_CHAN, app_listener, 0);

/* Forward declarations */
static void timer_work_fn(struct k_work *work);

/* Delayable work used to schedule triggers */
static K_WORK_DELAYABLE_DEFINE(trigger_work, timer_work_fn);

/* Forward declarations of state handlers */
static void running_entry(void *o);
static void running_run(void *o);

static void triggering_entry(void *o);
static void triggering_run(void *o);

static void requesting_location_entry(void *o);
static void requesting_location_run(void *o);

static void requesting_sensors_and_polling_entry(void *o);
static void requesting_sensors_and_polling_run(void *o);
static void requesting_sensors_and_polling_exit(void *o);

static void idle_entry(void *o);
static void idle_run(void *o);

static void fota_entry(void *o);
static void fota_run(void *o);

static void fota_downloading_run(void *o);

static void fota_network_disconnect_entry(void *o);
static void fota_network_disconnect_run(void *o);

static void fota_applying_image_entry(void *o);
static void fota_applying_image_run(void *o);

static void fota_rebooting_entry(void *o);

enum state {
	/* Normal operation */
	STATE_RUNNING,
		/* Triggers are periodically sent at a configured interval */
		STATE_TRIGGERING,
			/* Requesting location from the location module */
			STATE_REQUESTING_LOCATION,
			/* Requesting sensor values and polling for downlink data */
			STATE_REQUESTING_SENSORS_AND_POLLING,
		/* Disconnected from the network, no triggers are sent */
		STATE_IDLE,
	/* Ongoing FOTA process, triggers are blocked */
	STATE_FOTA,
		/* FOTA image is being downloaded */
		STATE_FOTA_DOWNLOADING,
		/* Disconnecting from the network */
		STATE_FOTA_NETWORK_DISCONNECT,
		/* Applying the image */
		STATE_FOTA_APPLYING_IMAGE,
		/* Rebooting */
		STATE_FOTA_REBOOTING,
};

/* State object for the app module.
 * Used to transfer data between state changes.
 */
struct app_state_object {
	/* This must be first */
	struct smf_ctx ctx;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;

	/* Trigger interval */
	uint64_t interval_sec;

	/* Button number */
	uint8_t button_number;

	/* Time available */
	enum time_status time_status;

	/* Cloud status */
	enum cloud_msg_type status;

	/* FOTA status */
	enum fota_msg_type fota_status;

	/* Network status */
	enum network_msg_type network_status;

	/* Location status */
	enum location_msg_type location_status;
};

static struct app_state_object app_state;

/* Construct state table */
static const struct smf_state states[] = {
	[STATE_RUNNING] = SMF_CREATE_STATE(
		running_entry,
		running_run,
		NULL,
		NULL,
		NULL
	),
	[STATE_TRIGGERING] = SMF_CREATE_STATE(
		triggering_entry,
		triggering_run,
		NULL,
		&states[STATE_RUNNING],
		&states[STATE_REQUESTING_LOCATION]
	),
	[STATE_REQUESTING_LOCATION] = SMF_CREATE_STATE(
		requesting_location_entry,
		requesting_location_run,
		NULL,
		&states[STATE_TRIGGERING],
		NULL
	),
	[STATE_REQUESTING_SENSORS_AND_POLLING] = SMF_CREATE_STATE(
		requesting_sensors_and_polling_entry,
		requesting_sensors_and_polling_run,
		requesting_sensors_and_polling_exit,
		&states[STATE_TRIGGERING],
		NULL
	),
	[STATE_IDLE] = SMF_CREATE_STATE(
		idle_entry,
		idle_run,
		NULL,
		&states[STATE_RUNNING],
		NULL
	),
	[STATE_FOTA] = SMF_CREATE_STATE(
		fota_entry,
		fota_run,
		NULL,
		NULL,
		&states[STATE_FOTA_DOWNLOADING]
	),
	[STATE_FOTA_DOWNLOADING] = SMF_CREATE_STATE(
		NULL,
		fota_downloading_run,
		NULL,
		&states[STATE_FOTA],
		NULL
	),
	[STATE_FOTA_NETWORK_DISCONNECT] = SMF_CREATE_STATE(
		fota_network_disconnect_entry,
		fota_network_disconnect_run,
		NULL,
		&states[STATE_FOTA],
		NULL
	),
	[STATE_FOTA_APPLYING_IMAGE] = SMF_CREATE_STATE(
		fota_applying_image_entry,
		fota_applying_image_run,
		NULL,
		&states[STATE_FOTA],
		NULL
	),
	[STATE_FOTA_REBOOTING] = SMF_CREATE_STATE(
		fota_rebooting_entry,
		NULL,
		NULL,
		&states[STATE_FOTA],
		NULL
	),
};

static void sensor_and_poll_triggers_send(void)
{
	int err;

#if defined(CONFIG_APP_REQUEST_NETWORK_QUALITY)
	struct network_msg network_msg = {
		.type = NETWORK_QUALITY_SAMPLE_REQUEST,
	};

	err = zbus_chan_pub(&NETWORK_CHAN, &network_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
#endif /* CONFIG_APP_REQUEST_NETWORK_QUALITY */

#if defined(CONFIG_APP_BATTERY)
	struct power_msg power_msg = {
		.type = POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST,
	};

	err = zbus_chan_pub(&POWER_CHAN, &power_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
#endif /* CONFIG_APP_BATTERY */

#if defined(CONFIG_APP_ENVIRONMENTAL)
	struct environmental_msg environmental_msg = {
		.type = ENVIRONMENTAL_SENSOR_SAMPLE_REQUEST,
	};

	err = zbus_chan_pub(&ENVIRONMENTAL_CHAN, &environmental_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
#endif /* CONFIG_APP_ENVIRONMENTAL */

	/* Send FOTA poll trigger */
	enum fota_msg_type fota_msg = FOTA_POLL_REQUEST;

	err = zbus_chan_pub(&FOTA_CHAN, &fota_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub FOTA trigger, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	/* Send trigger for shadow polling */
	struct cloud_msg cloud_msg = {
		.type = CLOUD_POLL_SHADOW
	};

	err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub shadow trigger, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

/* Delayable work used to send messages on the TIMER_CHAN */
static void timer_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	int err, dummy = 0;

	err = zbus_chan_pub(&TIMER_CHAN, &dummy, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

/* Zephyr State Machine framework handlers */

/* STATE_RUNNING */

static void running_entry(void *o)
{
	const struct app_state_object *state_object = (const struct app_state_object *)o;

	LOG_DBG("%s", __func__);

	if (state_object->status == CLOUD_CONNECTED_READY_TO_SEND ||
	    state_object->status == CLOUD_PAYLOAD_JSON ||
	    state_object->status == CLOUD_POLL_SHADOW) {
		STATE_SET(app_state, STATE_TRIGGERING);
		return;
	}

	STATE_SET(app_state, STATE_IDLE);
}

static void running_run(void *o)
{
	const struct app_state_object *state_object = (const struct app_state_object *)o;

	if (state_object->chan == &FOTA_CHAN &&
	    state_object->fota_status == FOTA_DOWNLOADING_UPDATE) {
		STATE_SET(app_state, STATE_FOTA);
		return;
	}
}

/* STATE_IDLE */

static void idle_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

#if defined(CONFIG_APP_LED)
	/* Blink Yellow */
	struct led_msg led_msg = {
		.type = LED_RGB_SET,
		.red = 255,
		.green = 255,
		.blue = 0,
		.duration_on_msec = 250,
		.duration_off_msec = 2000,
		.repetitions = 10,
	};

	int err = zbus_chan_pub(&LED_CHAN, &led_msg, K_SECONDS(1));

	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
#endif /* CONFIG_APP_LED */

	k_work_cancel_delayable(&trigger_work);
}

static void idle_run(void *o)
{
	const struct app_state_object *state_object = (const struct app_state_object *)o;

	if ((state_object->chan == &CLOUD_CHAN) &&
		(state_object->status == CLOUD_CONNECTED_READY_TO_SEND)) {
		STATE_SET(app_state, STATE_TRIGGERING);
		return;
	}
}

/* STATE_CONNECTED */

static void triggering_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

#if defined(CONFIG_APP_LED)
	/* Blink Green */
	struct led_msg led_msg = {
		.type = LED_RGB_SET,
		.red = 0,
		.green = 255,
		.blue = 0,
		.duration_on_msec = 250,
		.duration_off_msec = 2000,
		.repetitions = 10,
	};

	int err = zbus_chan_pub(&LED_CHAN, &led_msg, K_SECONDS(1));

	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
#endif /* CONFIG_APP_LED */

	k_work_reschedule(&trigger_work, K_NO_WAIT);
}

static void triggering_run(void *o)
{
	const struct app_state_object *state_object = (const struct app_state_object *)o;

	if ((state_object->chan == &CLOUD_CHAN) &&
	    ((state_object->status == CLOUD_CONNECTED_PAUSED) ||
	     (state_object->status == CLOUD_DISCONNECTED))) {
		STATE_SET(app_state, STATE_IDLE);
		return;
	}

	if (state_object->chan == &CONFIG_CHAN) {
		LOG_DBG("Configuration update, new interval: %lld", state_object->interval_sec);
		k_work_reschedule(&trigger_work, K_SECONDS(state_object->interval_sec));
		return;
	}
}

/* STATE_REQUESTING_LOCATION */

static void requesting_location_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	int err;
	enum location_msg_type location_msg = LOCATION_SEARCH_TRIGGER;

	err = zbus_chan_pub(&LOCATION_CHAN, &location_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub data sample trigger, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

static void requesting_location_run(void *o)
{
	const struct app_state_object *state_object = (const struct app_state_object *)o;

	if (state_object->chan == &LOCATION_CHAN &&
	    (state_object->location_status == LOCATION_SEARCH_DONE)) {
		STATE_SET(app_state, STATE_REQUESTING_SENSORS_AND_POLLING);
		return;
	}

	if (state_object->chan == &BUTTON_CHAN) {
		STATE_EVENT_HANDLED(app_state);
		return;
	}
}

/* STATE_REQUESTING_SENSORS_AND_POLLING */

static void requesting_sensors_and_polling_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	sensor_and_poll_triggers_send();

	LOG_DBG("Next trigger in %lld seconds", app_state.interval_sec);

	k_work_reschedule(&trigger_work, K_SECONDS(app_state.interval_sec));
}

static void requesting_sensors_and_polling_run(void *o)
{
	const struct app_state_object *state_object = (const struct app_state_object *)o;

	if (state_object->chan == &TIMER_CHAN) {
		STATE_SET(app_state, STATE_REQUESTING_LOCATION);
		return;
	}

	if (state_object->chan == &BUTTON_CHAN) {
		STATE_SET(app_state, STATE_REQUESTING_LOCATION);
		return;
	}
}

static void requesting_sensors_and_polling_exit(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	k_work_cancel_delayable(&trigger_work);
}

/* STATE_FOTA */

static void fota_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	k_work_cancel_delayable(&trigger_work);
}

static void fota_run(void *o)
{
	const struct app_state_object *state_object = (const struct app_state_object *)o;

	if (state_object->chan == &FOTA_CHAN) {
		switch (state_object->fota_status) {
		case FOTA_DOWNLOAD_CANCELED:
			__fallthrough;
		case FOTA_DOWNLOAD_TIMED_OUT:
			__fallthrough;
		case FOTA_DOWNLOAD_FAILED:
			STATE_SET(app_state, STATE_RUNNING);
			return;
		default:
			/* Don't care */
			break;
		}
	}
}

/* STATE_FOTA_DOWNLOADING */

static void fota_downloading_run(void *o)
{
	const struct app_state_object *state_object = (const struct app_state_object *)o;

	if (state_object->chan == &FOTA_CHAN) {
		switch (state_object->fota_status) {
		case FOTA_SUCCESS_REBOOT_NEEDED:
			STATE_SET(app_state, STATE_FOTA_NETWORK_DISCONNECT);
			return;
		case FOTA_IMAGE_APPLY_NEEDED:
			STATE_SET(app_state, STATE_FOTA_APPLYING_IMAGE);
			return;
		default:
			/* Don't care */
			break;
		}
	}
}

/* STATE_FOTA_NETWORK_DISCONNECT */

static void fota_network_disconnect_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	int err;
	struct network_msg msg = {
		.type = NETWORK_DISCONNECT
	};

	err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void fota_network_disconnect_run(void *o)
{
	const struct app_state_object *state_object = (const struct app_state_object *)o;

	if (state_object->chan == &NETWORK_CHAN &&
	    state_object->network_status == NETWORK_DISCONNECTED) {
		STATE_SET(app_state, STATE_FOTA_REBOOTING);
		return;
	}
}

/* STATE_FOTA_APPLYING_IMAGE, */

static void fota_applying_image_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	int err;
	struct network_msg msg = {
		.type = NETWORK_DISCONNECT
	};

	err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void fota_applying_image_run(void *o)
{
	const struct app_state_object *state_object = (const struct app_state_object *)o;

	if (state_object->chan == &NETWORK_CHAN &&
	    state_object->network_status == NETWORK_DISCONNECTED) {

		int err;
		enum fota_msg_type msg = FOTA_IMAGE_APPLY;

		err = zbus_chan_pub(&FOTA_CHAN, &msg, K_SECONDS(1));
		if (err) {
			LOG_ERR("zbus_chan_pub, error: %d", err);
			SEND_FATAL_ERROR();
		}

	} else if (state_object->chan == &FOTA_CHAN &&
		   state_object->fota_status == FOTA_SUCCESS_REBOOT_NEEDED) {
		STATE_SET(app_state, STATE_FOTA_REBOOTING);
		return;
	}
}

/* STATE_FOTA_REBOOTING */

static void fota_rebooting_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	/* Reboot the device */
	LOG_WRN("Rebooting the device to apply the FOTA update");

	/* Flush log buffer */
	LOG_PANIC();

	k_sleep(K_SECONDS(5));

	sys_reboot(SYS_REBOOT_COLD);
}

/* Function called when there is a message received on a channel that the module listens to */
static void app_callback(const struct zbus_channel *chan)
{
	int err;

	/* Update the state object with the channel that the message was received on */
	app_state.chan = chan;

	/* Copy corresponding data to the state object depending on the incoming channel */
	if (&CONFIG_CHAN == chan) {
		const struct configuration *config = zbus_chan_const_msg(chan);

		if (config->update_interval_present) {
			app_state.interval_sec = config->update_interval;
		}
	} else if (&CLOUD_CHAN == chan) {
		const struct cloud_msg *cloud_msg = zbus_chan_const_msg(chan);

		app_state.status = cloud_msg->type;
	} else if (&FOTA_CHAN == chan) {
		const enum fota_msg_type *fota_status = zbus_chan_const_msg(chan);

		app_state.fota_status = *fota_status;
	} else if (&NETWORK_CHAN == chan) {
		const struct network_msg *network_msg = zbus_chan_const_msg(chan);

		app_state.network_status = network_msg->type;
	} else if (&LOCATION_CHAN == chan) {
		const enum location_msg_type *location_msg = zbus_chan_const_msg(chan);

		app_state.location_status = *location_msg;
	}

	/* State object updated, run SMF */
	err = STATE_RUN(app_state);
	if (err) {
		LOG_ERR("smf_run_state, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

static int app_init(void)
{
	app_state.interval_sec = CONFIG_APP_MODULE_TRIGGER_TIMEOUT_SECONDS;

	STATE_SET_INITIAL(app_state, STATE_RUNNING);

	return 0;
}

SYS_INIT(app_init, POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY);
