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
#include "battery.h"
#endif /* CONFIG_APP_BATTERY */

/* Register log module */
LOG_MODULE_REGISTER(app, CONFIG_APP_LOG_LEVEL);

/* Define a ZBUS listener for this module */
static void app_callback(const struct zbus_channel *chan);
ZBUS_LISTENER_DEFINE(app_listener, app_callback);

/* Observe channels */
ZBUS_CHAN_ADD_OBS(CONFIG_CHAN, app_listener, 0);
ZBUS_CHAN_ADD_OBS(CLOUD_CHAN, app_listener, 0);
ZBUS_CHAN_ADD_OBS(BUTTON_CHAN, app_listener, 0);
ZBUS_CHAN_ADD_OBS(FOTA_CHAN, app_listener, 0);
ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, app_listener, 0);

/* Forward declarations */
static void trigger_work_fn(struct k_work *work);

/* Delayable work used to schedule triggers */
static K_WORK_DELAYABLE_DEFINE(trigger_work, trigger_work_fn);

/* Forward declarations of state handlers */
static void running_run(void *o);

static void periodic_triggering_entry(void *o);
static void periodic_triggering_run(void *o);

static void idle_entry(void *o);
static void idle_run(void *o);

static void fota_entry(void *o);
static void fota_run(void *o);

static void fota_network_disconnect_pending_entry(void *o);
static void fota_network_disconnect_pending_run(void *o);

static void fota_image_apply_pending_entry(void *o);
static void fota_image_apply_pending_run(void *o);

static void fota_rebooting_entry(void *o);

/* Defining the hierarchical trigger module states:
 * STATE_RUNNING: The module is operating normally.
 *	STATE_PERIODIC_TRIGGERING: The module is sending triggers to the rest of the system.
 *	STATE_IDLE: The module is disconnected from the cloud, triggers are blocked.
 * STATE_FOTA: The module is in the FOTA process, triggers are blocked.
 *	STATE_FOTA_NETWORK_DISCONNECT_PENDING: The module is waiting for the network to disconnect.
 *	STATE_FOTA_IMAGE_APPLY_PENDING: The module is waiting for the FOTA image to be applied.
 *	STATE_FOTA_REBOOTING: The module is rebooting after applying the FOTA image.
 */
enum state {
	STATE_RUNNING,
	STATE_PERIODIC_TRIGGERING,
	STATE_IDLE,
	STATE_FOTA,
	STATE_FOTA_NETWORK_DISCONNECT_PENDING,
	STATE_FOTA_IMAGE_APPLY_PENDING,
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
};

static struct app_state_object app_state;

/* Construct state table */
static const struct smf_state states[] = {
	[STATE_RUNNING] = SMF_CREATE_STATE(
		NULL,
		running_run,
		NULL,
		NULL,
		&states[STATE_IDLE]
	),
	[STATE_PERIODIC_TRIGGERING] = SMF_CREATE_STATE(
		periodic_triggering_entry,
		periodic_triggering_run,
		NULL,
		&states[STATE_RUNNING],
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
		NULL
	),
	[STATE_FOTA_NETWORK_DISCONNECT_PENDING] = SMF_CREATE_STATE(
		fota_network_disconnect_pending_entry,
		fota_network_disconnect_pending_run,
		NULL,
		&states[STATE_FOTA],
		NULL
	),
	[STATE_FOTA_IMAGE_APPLY_PENDING] = SMF_CREATE_STATE(
		fota_image_apply_pending_entry,
		fota_image_apply_pending_run,
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

static void triggers_send(void)
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
	struct battery_msg battery_msg = {
		.type = BATTERY_PERCENTAGE_SAMPLE_REQUEST,
	};

	err = zbus_chan_pub(&BATTERY_CHAN, &battery_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
#endif /* CONFIG_APP_BATTERY */

#if defined(CONFIG_APP_ENVIRONMENTAL)
	struct battery_msg environmental_msg = {
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

	/* Trigger location search and environmental data sample */
	enum location_msg_type location_msg = LOCATION_SEARCH_TRIGGER;

	err = zbus_chan_pub(&LOCATION_CHAN, &location_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub data sample trigger, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

/* Delayable work used to send triggers to the rest of the system */
static void trigger_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_DBG("Sending data sample trigger");

	triggers_send();

	k_work_reschedule(&trigger_work, K_SECONDS(app_state.interval_sec));
}

/* Zephyr State Machine framework handlers */

/* STATE_RUNNING */

static void running_run(void *o)
{
	const struct app_state_object *state_object = (const struct app_state_object *)o;

	if (state_object->chan == &FOTA_CHAN) {
		if (state_object->fota_status == FOTA_DOWNLOADING_UPDATE) {
			STATE_SET(app_state, STATE_FOTA);
			return;
		}
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
		LOG_DBG("Cloud connected and ready, going into periodic triggering state");
		STATE_SET(app_state, STATE_PERIODIC_TRIGGERING);
		return;
	}
}

/* STATE_CONNECTED */

static void periodic_triggering_entry(void *o)
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

static void periodic_triggering_run(void *o)
{
	const struct app_state_object *state_object = (const struct app_state_object *)o;

	if ((state_object->chan == &CLOUD_CHAN) &&
		((state_object->status == CLOUD_CONNECTED_PAUSED) ||
		(state_object->status == CLOUD_DISCONNECTED))) {
		LOG_DBG("Cloud disconnected/paused, going into idle state");
		STATE_SET(app_state, STATE_IDLE);
		return;
	}

	if (state_object->chan == &BUTTON_CHAN) {
		k_work_reschedule(&trigger_work, K_NO_WAIT);
		return;
	}

	if (state_object->chan == &CONFIG_CHAN) {
		LOG_DBG("Configuration update, new interval: %lld", state_object->interval_sec);
		k_work_reschedule(&trigger_work, K_SECONDS(state_object->interval_sec));
		return;
	}
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
		case FOTA_CANCELED:
			__fallthrough;
		case FOTA_DOWNLOAD_TIMED_OUT:
			__fallthrough;
		case FOTA_DOWNLOAD_FAILED:
			STATE_SET(app_state, STATE_RUNNING);
			return;
		case FOTA_NETWORK_DISCONNECT_NEEDED:
			STATE_SET(app_state, STATE_FOTA_NETWORK_DISCONNECT_PENDING);
			return;
		default:
			/* Don't care */
			break;
		}
	}
}

/* STATE_FOTA_NETWORK_DISCONNECT_PENDING */

static void fota_network_disconnect_pending_entry(void *o)
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

static void fota_network_disconnect_pending_run(void *o)
{
	const struct app_state_object *state_object = (const struct app_state_object *)o;

	if (state_object->chan == &NETWORK_CHAN) {
		if (state_object->network_status == NETWORK_DISCONNECTED) {
			STATE_SET(app_state, STATE_FOTA_IMAGE_APPLY_PENDING);
			return;
		}
	}
}

/* STATE_FOTA_IMAGE_APPLY_PENDING */

static void fota_image_apply_pending_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	int err;
	enum fota_msg_type msg = FOTA_APPLY_IMAGE;

	err = zbus_chan_pub(&FOTA_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void fota_image_apply_pending_run(void *o)
{
	const struct app_state_object *state_object = (const struct app_state_object *)o;

	if (state_object->chan == &FOTA_CHAN) {
		if (state_object->fota_status == FOTA_REBOOT_NEEDED) {
			STATE_SET(app_state, STATE_FOTA_REBOOTING);
			return;
		}
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
		const enum cloud_msg_type *status = zbus_chan_const_msg(chan);

		app_state.status = *status;
	} else if (&FOTA_CHAN == chan) {
		const enum fota_msg_type *fota_status = zbus_chan_const_msg(chan);

		app_state.fota_status = *fota_status;
	} else if (&NETWORK_CHAN == chan) {
		const struct network_msg *network_msg = zbus_chan_const_msg(chan);

		app_state.network_status = network_msg->type;
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
