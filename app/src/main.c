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
#include <zephyr/sys/reboot.h>

#include "app_common.h"
#include "button.h"
#include "modules/button/button.h"
#include "modules/storage/storage.h"
#include "network.h"
#include "cloud.h"
#include "fota.h"
#include "location.h"
#include "storage.h"
#include "cbor_helper.h"

#if defined(CONFIG_APP_LED)
#include "led.h"
#endif /* CONFIG_APP_LED */

#if defined(CONFIG_APP_ENVIRONMENTAL)
#include "environmental.h"
#endif /* CONFIG_APP_ENVIRONMENTAL */

#if defined(CONFIG_APP_POWER)
#include "power.h"
#endif /* CONFIG_APP_POWER */

/* Register log module */
LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

/* Configuration constants to replace magic numbers */
#define INITIAL_DATA_SEND_DELAY_SEC	1
#define ZBUS_PUBLISH_TIMEOUT_MS		100

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(main_subscriber);

enum timer_msg_type {
	/* Timer for sampling data has expired.
	 * This timer is used to trigger the sampling of data from the sensors.
	 * The timer is set to expire every CONFIG_APP_MODULE_TRIGGER_TIMEOUT_SECONDS seconds,
	 * and can be overridden from the cloud.
	 * If the storage module is in passthrough mode, expiry of this timer will also trigger
	 * polling of the cloud shadow and FOTA status.
	 */
	TIMER_EXPIRED_SAMPLE_DATA,

	/* Timer for cloud polling and data sending has expired.
	 * This timer is used to trigger the cloud shadow and FOTA status polling and data sending.
	 * The timer is set to expire every CONFIG_APP_STORAGE_DATA_SEND_INTERVAL_SECONDS seconds,
	 * and can be overridden from the cloud.
	 */
	TIMER_EXPIRED_CLOUD,
};

ZBUS_CHAN_DEFINE(TIMER_CHAN,
		 enum timer_msg_type,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Macro to extract the timer message type from the message buffer.
 * @param msg The message buffer to extract the type from.
 */
#define MSG_TO_TIMER_TYPE(msg)	(*(const enum timer_msg_type *)msg)

/* Define the channels that the module subscribes to, their associated message types
 * and the subscriber that will receive the messages on the channel.
 * We use the X-macros to make the code more maintainable.
 */
#define CHANNEL_LIST(X)						\
	X(CLOUD_CHAN,		struct cloud_msg)		\
	X(BUTTON_CHAN,		struct button_msg)		\
	X(FOTA_CHAN,		enum fota_msg_type)		\
	X(NETWORK_CHAN,		struct network_msg)		\
	X(LOCATION_CHAN,	struct location_msg)		\
	X(STORAGE_CHAN,		struct storage_msg)		\
	X(TIMER_CHAN,		enum timer_msg_type)

/* Calculate the maximum message size from the list of channels */
#define MAX_MSG_SIZE				MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

/* Add main_subscriber as observer to all the channels in the list. */
#define ADD_OBSERVERS(_chan, _type)		ZBUS_CHAN_ADD_OBS(_chan, main_subscriber, 0);

/*
 * Expand to a call to ZBUS_CHAN_ADD_OBS for each channel in the list.
 * Example: ZBUS_CHAN_ADD_OBS(CLOUD_CHAN, main_subscriber, 0);
 */
CHANNEL_LIST(ADD_OBSERVERS)

/* Forward declarations */
struct main_state;  /* Forward declaration of main_state struct */

static void timer_send_data_work_fn(struct k_work *work);
static void timer_sample_data_work_fn(struct k_work *work);
static void timer_sample_start(uint32_t delay_sec);
static void timer_send_data_start(uint32_t delay_sec);

/* Helper functions for running_run() */
static void handle_cloud_connected(struct main_state *state_object);
static void handle_cloud_disconnected(struct main_state *state_object);
static void handle_cloud_shadow_response(struct main_state *state_object,
					 const struct cloud_msg *msg);
static void handle_timer_expired_cloud(struct main_state *state_object);
static void handle_button_press_long(struct main_state *state_object);

/* Delayable work used to schedule triggers */
static K_WORK_DELAYABLE_DEFINE(timer_send_data_work, timer_send_data_work_fn);
static K_WORK_DELAYABLE_DEFINE(timer_sample_data_work, timer_sample_data_work_fn);

/* Forward declarations of state handlers */
static void running_entry(void *o);
static void running_run(void *o);

static void sample_data_entry(void *o);
static void sample_data_run(void *o);

static void wait_for_trigger_entry(void *o);
static void wait_for_trigger_run(void *o);
static void wait_for_trigger_exit(void *o);

static void fota_entry(void *o);
static void fota_run(void *o);

static void fota_downloading_entry(void *o);
static void fota_downloading_run(void *o);

static void fota_waiting_for_network_disconnect_entry(void *o);
static void fota_waiting_for_network_disconnect_run(void *o);

static void fota_waiting_for_network_disconnect_to_apply_image_entry(void *o);
static void fota_waiting_for_network_disconnect_to_apply_image_run(void *o);

static void fota_applying_image_entry(void *o);
static void fota_applying_image_run(void *o);

static void fota_rebooting_entry(void *o);

enum state {
	/* Normal operation */
	STATE_RUNNING,
		/* Requesting data samples from relevant modules.
		 * Location data is requested first, upon state entry.
		 * After location data is received, the other modules are polled.
		 */
		STATE_SAMPLE_DATA,
		/* Wait for timer or button press to trigger the next sample */
		STATE_WAIT_FOR_TRIGGER,
	/* Ongoing FOTA process, triggers are blocked */
	STATE_FOTA,
		/* FOTA image is being downloaded */
		STATE_FOTA_DOWNLOADING,
		/* Disconnecting from the network */
		STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT,
		/* Waiting for network disconnect to apply the image, state needed for
		 * Full Modem FOTA. Extra step needed to apply the image before rebooting.
		 */
		STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT_TO_APPLY_IMAGE,
		/* Applying the image */
		STATE_FOTA_APPLYING_IMAGE,
		/* Rebooting */
		STATE_FOTA_REBOOTING,
};

/* State object for the app module.
 * Used to transfer data between state changes.
 */
struct main_state {
	/* This must be first */
	struct smf_ctx ctx;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;

	/* Last received message */
	uint8_t msg_buf[MAX_MSG_SIZE];

	/* Trigger interval */
	uint32_t sample_interval_sec;

	/* Data sending interval */
	uint32_t data_send_interval_sec;

	/* Cloud connection status */
	bool connected;

	/* Start time of the most recent sampling. This is used to calculate the correct
	 * time when scheduling the next sampling trigger.
	 */
	uint32_t sample_start_time;

	/* Storage batch session ID for batch operations */
	uint32_t storage_session_id;

	/* Data sending queued.
	 * This is used to queue up a data send when the device is not connected to the network.
	 */
	bool data_sending_queued;

	/* Poll trigger queued.
	 * This is used to queue up a poll trigger when the device is not connected to the network
	 * or if it is currently doing a location search.
	 */
	bool poll_trigger_queued;

	/* Passthrough mode */
	bool passthrough_mode;
};

/* Construct state table */
static const struct smf_state states[] = {
	[STATE_RUNNING] = SMF_CREATE_STATE(
		running_entry,			/* Entry function */
		running_run,			/* Run function */
		NULL,				/* Exit function */
		NULL,				/* Parent state */
		&states[STATE_SAMPLE_DATA]	/* Initial transition */
	),
	[STATE_SAMPLE_DATA] = SMF_CREATE_STATE(
		sample_data_entry,
		sample_data_run,
		NULL,
		&states[STATE_RUNNING],
		NULL
	),
	[STATE_WAIT_FOR_TRIGGER] = SMF_CREATE_STATE(
		wait_for_trigger_entry,
		wait_for_trigger_run,
		wait_for_trigger_exit,
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
		fota_downloading_entry,
		fota_downloading_run,
		NULL,
		&states[STATE_FOTA],
		NULL
	),
	[STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT] = SMF_CREATE_STATE(
		fota_waiting_for_network_disconnect_entry,
		fota_waiting_for_network_disconnect_run,
		NULL,
		&states[STATE_FOTA],
		NULL
	),
	[STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT_TO_APPLY_IMAGE] = SMF_CREATE_STATE(
		fota_waiting_for_network_disconnect_to_apply_image_entry,
		fota_waiting_for_network_disconnect_to_apply_image_run,
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

/* Static helper function */

static void task_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void poll_triggers_send(void)
{
	int err;
	struct cloud_msg cloud_msg = {
		.type = CLOUD_POLL_SHADOW
	};
	enum fota_msg_type fota_msg = FOTA_POLL_REQUEST;

	err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to publish cloud shadow poll trigger, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	err = zbus_chan_pub(&FOTA_CHAN, &fota_msg, K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to publish FOTA poll trigger, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static void sensor_triggers_send(void)
{
	int err;

	(void)err;

#if defined(CONFIG_APP_REQUEST_NETWORK_QUALITY)
	struct network_msg network_msg = {
		.type = NETWORK_QUALITY_SAMPLE_REQUEST,
	};

	err = zbus_chan_pub(&NETWORK_CHAN, &network_msg, K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to publish network quality sample request, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
#endif /* CONFIG_APP_REQUEST_NETWORK_QUALITY */

#if defined(CONFIG_APP_POWER)
	struct power_msg power_msg = {
		.type = POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST,
	};

	err = zbus_chan_pub(&POWER_CHAN, &power_msg, K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to publish power battery sample request, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
#endif /* CONFIG_APP_POWER */

#if defined(CONFIG_APP_ENVIRONMENTAL)
	struct environmental_msg environmental_msg = {
		.type = ENVIRONMENTAL_SENSOR_SAMPLE_REQUEST,
	};

	err = zbus_chan_pub(&ENVIRONMENTAL_CHAN, &environmental_msg,
			    K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to publish environmental sensor sample request, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
#endif /* CONFIG_APP_ENVIRONMENTAL */
}

static void storage_send_data(struct main_state *state_object)
{
	int err;
	struct storage_msg storage_msg = {
		.type = STORAGE_BATCH_REQUEST,
	};

	state_object->storage_session_id = k_uptime_get_32();
	storage_msg.session_id = state_object->storage_session_id;

	err = zbus_chan_pub(&STORAGE_CHAN, &storage_msg, K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to publish storage batch request (session: %u), error: %d",
			storage_msg.session_id, err);
		SEND_FATAL_ERROR();

		return;
	}
}

/* Delayable work used to send messages on the TIMER_CHAN */
static void timer_send_data_work_fn(struct k_work *work)
{
	int err;
	enum timer_msg_type msg = TIMER_EXPIRED_CLOUD;

	ARG_UNUSED(work);

	err = zbus_chan_pub(&TIMER_CHAN, &msg, K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to publish cloud timer expired message, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static void timer_sample_data_work_fn(struct k_work *work)
{
	int err;
	enum timer_msg_type msg = TIMER_EXPIRED_SAMPLE_DATA;

	ARG_UNUSED(work);

	err = zbus_chan_pub(&TIMER_CHAN, &msg, K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to publish sample data timer expired message, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static void timer_sample_start(uint32_t delay_sec)
{
	int err;

	err = k_work_reschedule(&timer_sample_data_work, K_SECONDS(delay_sec));
	if (err < 0) {
		LOG_ERR("k_work_reschedule timer_sample_data_work, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void timer_send_data_start(uint32_t delay_sec)
{
	int err;

	err = k_work_reschedule(&timer_send_data_work, K_SECONDS(delay_sec));
	if (err < 0) {
		LOG_ERR("k_work_reschedule timer_send_data_work, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

/* Helper functions for running_run() state handler */

/**
 * @brief Handle cloud connected event in running state
 *
 * Processes queued operations and handles passthrough mode behavior when
 * the cloud connection is established.
 */
static void handle_cloud_connected(struct main_state *state_object)
{
	state_object->connected = true;

	/* Process any queued poll triggers */
	if (state_object->poll_trigger_queued) {
		poll_triggers_send();
		state_object->poll_trigger_queued = false;
	}

	/* Process any queued data sending operations */
	if (state_object->data_sending_queued) {
		storage_send_data(state_object);
		state_object->data_sending_queued = false;
	}

	/* In passthrough mode, trigger immediate sampling when connected */
	if (state_object->passthrough_mode) {
		LOG_DBG("Passthrough mode: triggering immediate sampling on connection");
		smf_set_state(SMF_CTX(state_object), &states[STATE_SAMPLE_DATA]);
	}
}

/**
 * @brief Handle cloud disconnected event in running state
 *
 * Cancels sampling timers in passthrough mode to conserve power when
 * no connectivity is available.
 */
static void handle_cloud_disconnected(struct main_state *state_object)
{
	state_object->connected = false;

	/* In passthrough mode, stop sampling when disconnected to save power */
	if (state_object->passthrough_mode) {
		(void)k_work_cancel_delayable(&timer_sample_data_work);
	}
}

/**
 * @brief Handle cloud shadow response in running state
 *
 * Processes shadow updates to adjust sampling intervals based on
 * cloud configuration.
 */
static void handle_cloud_shadow_response(struct main_state *state_object,
					const struct cloud_msg *msg)
{
	int err;

	err = get_update_interval_from_cbor_response(
		msg->response.buffer,
		msg->response.buffer_data_len,
		&state_object->sample_interval_sec);
	if (err) {
		LOG_ERR("Failed to parse shadow response interval, error: %d", err);
		return;
	}

	LOG_WRN("Received new sampling interval: %d seconds",
		state_object->sample_interval_sec);

	timer_sample_start(state_object->sample_interval_sec);
}

/**
 * @brief Handle timer expired cloud event in running state
 *
 * Triggers cloud polling and data sending operations, or queues them
 * if not currently connected.
 */
static void handle_timer_expired_cloud(struct main_state *state_object)
{
	if (state_object->connected) {
		poll_triggers_send();
		storage_send_data(state_object);
	} else {
		LOG_DBG("Cloud timer expired while disconnected, queueing data send");
		state_object->data_sending_queued = true;
	}
}

/**
 * @brief Handle long button press event in running state
 *
 * Triggers immediate polling and data sending operations, or queues them
 * if not currently connected.
 */
static void handle_button_press_long(struct main_state *state_object)
{
	LOG_DBG("Long button press detected, attempting to poll and send data immediately");

	if (state_object->connected) {
		poll_triggers_send();
		storage_send_data(state_object);
	} else {
		LOG_DBG("Long button press while disconnected, queueing operations");
		state_object->data_sending_queued = true;
	}
}

/* Zephyr State Machine framework handlers */

/* STATE_RUNNING */

static void running_entry(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	LOG_DBG("%s", __func__);

	if (IS_ENABLED(CONFIG_APP_STORAGE_INITIAL_MODE_BUFFER)) {
		timer_send_data_start(INITIAL_DATA_SEND_DELAY_SEC);
	}

	timer_sample_start(state_object->sample_interval_sec);
}

static void running_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	/* Handle cloud channel messages */
	if (state_object->chan == &CLOUD_CHAN) {
		const struct cloud_msg *msg = MSG_TO_CLOUD_MSG_PTR(state_object->msg_buf);

		switch (msg->type) {
		case CLOUD_CONNECTED:
			handle_cloud_connected(state_object);
			return;
		case CLOUD_DISCONNECTED:
			handle_cloud_disconnected(state_object);
			break;
		case CLOUD_SHADOW_RESPONSE:
			handle_cloud_shadow_response(state_object, msg);
			break;
		default:
			break;
		}
	}

	/* Handle FOTA download initiation */
	if (state_object->chan == &FOTA_CHAN &&
	    MSG_TO_FOTA_TYPE(state_object->msg_buf) == FOTA_DOWNLOADING_UPDATE) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_FOTA]);
		return;
	}

	/* Handle cloud data sending timer */
	if (state_object->chan == &TIMER_CHAN &&
	    MSG_TO_TIMER_TYPE(state_object->msg_buf) == TIMER_EXPIRED_CLOUD) {
		handle_timer_expired_cloud(state_object);
		return;
	}

	/* Handle long button press for manual data sending */
	if (state_object->chan == &BUTTON_CHAN &&
	    MSG_TO_BUTTON_MSG(state_object->msg_buf).type == BUTTON_PRESS_LONG) {
		handle_button_press_long(state_object);
		return;
	}
}

/* STATE_SAMPLE_DATA */

static void sample_data_entry(void *o)
{
	int err;
	struct location_msg location_msg = {
		.type = LOCATION_SEARCH_TRIGGER,
	};
	struct main_state *state_object = (struct main_state *)o;

	LOG_DBG("%s", __func__);

#if defined(CONFIG_APP_LED)
	/* Green pattern during active sampling */
	struct led_msg led_msg = {
		.type = LED_RGB_SET,
		.red = 0,
		.green = 55,
		.blue = 0,
		.duration_on_msec = 250,
		.duration_off_msec = 2000,
		.repetitions = 10,
	};

	err = zbus_chan_pub(&LED_CHAN, &led_msg, K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to publish LED pattern message, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
#endif /* CONFIG_APP_LED */

	if (state_object->connected) {
		/* Record the start time of sampling */
		state_object->sample_start_time = k_uptime_seconds();

		err = zbus_chan_pub(&LOCATION_CHAN, &location_msg, K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));
		if (err) {
			LOG_ERR("Failed to publish location search trigger, error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}
	}
}

static void sample_data_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	if (state_object->chan == &LOCATION_CHAN &&
	    MSG_TO_LOCATION_TYPE(state_object->msg_buf) == LOCATION_SEARCH_DONE) {

		/* In passthrough mode, send data immediately after sampling */
		if (state_object->passthrough_mode && state_object->connected) {
			sensor_triggers_send();
			poll_triggers_send();
		}

		if (!state_object->passthrough_mode) {
			sensor_triggers_send();
		}

		smf_set_state(SMF_CTX(state_object), &states[STATE_WAIT_FOR_TRIGGER]);

		return;
	}

	/* We are already sampling, ignore any new triggers */
	if (state_object->chan == &BUTTON_CHAN &&
	    MSG_TO_BUTTON_MSG(state_object->msg_buf).type == BUTTON_PRESS_SHORT) {
		smf_set_handled(SMF_CTX(state_object));

		return;
	}

	if (state_object->chan == &TIMER_CHAN) {
		enum timer_msg_type msg = MSG_TO_TIMER_TYPE(state_object->msg_buf);

		if (msg == TIMER_EXPIRED_SAMPLE_DATA) {
			smf_set_handled(SMF_CTX(state_object));

			return;
		}

		if (msg == TIMER_EXPIRED_CLOUD) {
			state_object->data_sending_queued = true;

			smf_set_handled(SMF_CTX(state_object));

			return;
		}
	}
}

/* STATE_WAIT_FOR_TRIGGER */

static void wait_for_trigger_entry(void *o)
{
	struct main_state *state_object = (struct main_state *)o;
	uint32_t time_elapsed = k_uptime_seconds() - state_object->sample_start_time;
	uint32_t time_remaining;

	if (time_elapsed > state_object->sample_interval_sec) {
		LOG_WRN("Sampling took longer than the interval, skipping next trigger");

		time_remaining = 0;
	} else {
		time_remaining = state_object->sample_interval_sec - time_elapsed;
	}

	LOG_DBG("%s", __func__);

	LOG_DBG("Next trigger in %d seconds", time_remaining);

	timer_sample_start(time_remaining);

#if defined(CONFIG_APP_LED)
	int err;
	/* Blue pattern for wait state */
	struct led_msg led_msg = {
		.type = LED_RGB_SET,
		.red = 0,
		.green = 0,
		.blue = 55,
		.duration_on_msec = 250,
		.duration_off_msec = 2000,
		.repetitions = 10,
	};

	err = zbus_chan_pub(&LED_CHAN, &led_msg, K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to publish LED wait state pattern, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
#endif /* CONFIG_APP_LED */

	/* If data sending is queued, send it */
	if (state_object->data_sending_queued) {
		poll_triggers_send();
		storage_send_data(state_object);

		return;
	}
}

static void wait_for_trigger_run(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	if (state_object->chan == &TIMER_CHAN &&
	    MSG_TO_TIMER_TYPE(state_object->msg_buf) == TIMER_EXPIRED_SAMPLE_DATA) {
		/* In passthrough mode, only sample when connected */
		if (state_object->passthrough_mode && !state_object->connected) {
			LOG_DBG("Passthrough mode: skipping sampling while disconnected");
			/* Restart timer for next attempt */
			timer_sample_start(state_object->sample_interval_sec);
			return;
		}

		smf_set_state(SMF_CTX(state_object), &states[STATE_SAMPLE_DATA]);

		return;
	}

	if (state_object->chan == &BUTTON_CHAN &&
	    MSG_TO_BUTTON_MSG(state_object->msg_buf).type == BUTTON_PRESS_SHORT) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_SAMPLE_DATA]);
		return;
	}
}

static void wait_for_trigger_exit(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	(void)k_work_cancel_delayable(&timer_sample_data_work);
}

/* STATE_FOTA */

static void fota_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	(void)k_work_cancel_delayable(&timer_sample_data_work);
}

static void fota_run(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	if (state_object->chan == &FOTA_CHAN) {
		enum fota_msg_type msg = MSG_TO_FOTA_TYPE(state_object->msg_buf);

		switch (msg) {
		case FOTA_DOWNLOAD_CANCELED:
			__fallthrough;
		case FOTA_DOWNLOAD_TIMED_OUT:
			__fallthrough;
		case FOTA_DOWNLOAD_FAILED:
			smf_set_state(SMF_CTX(state_object), &states[STATE_RUNNING]);
			return;
		default:
			/* Don't care */
			break;
		}
	}
}

/* STATE_FOTA_DOWNLOADING */

static void fota_downloading_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

#if defined(CONFIG_APP_LED)
	int err;

	/* Purple pattern during download - indefinite for ongoing process */
	struct led_msg led_msg = {
		.type = LED_RGB_SET,
		.red = 160,
		.green = 32,
		.blue = 240,
		.duration_on_msec = 250,
		.duration_off_msec = 2000,
		.repetitions = -1,
	};

	err = zbus_chan_pub(&LED_CHAN, &led_msg, K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to publish LED FOTA download pattern, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
#endif /* CONFIG_APP_LED */
}

static void fota_downloading_run(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	if (state_object->chan == &FOTA_CHAN) {
		enum fota_msg_type msg = MSG_TO_FOTA_TYPE(state_object->msg_buf);

		switch (msg) {
		case FOTA_SUCCESS_REBOOT_NEEDED:
			smf_set_state(SMF_CTX(state_object),
					      &states[STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT]);
			return;
		case FOTA_IMAGE_APPLY_NEEDED:
			smf_set_state(SMF_CTX(state_object),
				&states[STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT_TO_APPLY_IMAGE]);
			return;
		default:
			/* Don't care */
			break;
		}
	}
}

/* STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT */

static void fota_waiting_for_network_disconnect_entry(void *o)
{
	int err;
	struct network_msg msg = {
		.type = NETWORK_DISCONNECT
	};

	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to publish network disconnect request, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void fota_waiting_for_network_disconnect_run(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	if (state_object->chan == &NETWORK_CHAN) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		if (msg.type == NETWORK_DISCONNECTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_FOTA_REBOOTING]);
			return;
		}
	}
}

/* STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT_TO_APPLY_IMAGE */

static void fota_waiting_for_network_disconnect_to_apply_image_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	int err;
	struct network_msg msg = {
		.type = NETWORK_DISCONNECT
	};

	err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to publish network disconnect request, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void fota_waiting_for_network_disconnect_to_apply_image_run(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	if (state_object->chan == &NETWORK_CHAN) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		if (msg.type == NETWORK_DISCONNECTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_FOTA_APPLYING_IMAGE]);
		}
	}
}

/* STATE_FOTA_APPLYING_IMAGE, */

static void fota_applying_image_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	int err;
	enum fota_msg_type msg = FOTA_IMAGE_APPLY;

	err = zbus_chan_pub(&FOTA_CHAN, &msg, K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to publish FOTA image apply request, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void fota_applying_image_run(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	if (state_object->chan == &FOTA_CHAN) {
		enum fota_msg_type msg = MSG_TO_FOTA_TYPE(state_object->msg_buf);

		if (msg == FOTA_SUCCESS_REBOOT_NEEDED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_FOTA_REBOOTING]);
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

int main(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = (CONFIG_APP_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	struct main_state main_state = { 0 };

	main_state.sample_interval_sec = CONFIG_APP_MODULE_TRIGGER_TIMEOUT_SECONDS;
	main_state.data_send_interval_sec = CONFIG_APP_STORAGE_DATA_SEND_INTERVAL_SECONDS;
	main_state.passthrough_mode = IS_ENABLED(CONFIG_APP_STORAGE_INITIAL_MODE_PASSTHROUGH);

	LOG_DBG("Main has started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, task_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		SEND_FATAL_ERROR();
		return -EFAULT;
	}

	smf_set_initial(SMF_CTX(&main_state), &states[STATE_RUNNING]);

	while (1) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			SEND_FATAL_ERROR();

			return err;
		}

		err = zbus_sub_wait_msg(&main_subscriber, &main_state.chan, main_state.msg_buf,
					zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			SEND_FATAL_ERROR();

			return err;
		}

		err = smf_run_state(SMF_CTX(&main_state));
		if (err) {
			LOG_ERR("smf_run_state(), error: %d", err);
			SEND_FATAL_ERROR();

			return err;
		}
	}
}
