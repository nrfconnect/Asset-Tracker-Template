/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdint.h>
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
LOG_MODULE_REGISTER(main, 4);

/* Configuration constants to replace magic numbers */
#define ZBUS_PUBLISH_TIMEOUT_MS		100

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(main_subscriber);

enum timer_msg_type {
	/* Timer for sampling data has expired.
	 * This timer is used to trigger the sampling of data from the sensors.
	 * The timer is set to expire every CONFIG_APP_SENSOR_SAMPLING_INTERVAL_SECONDS seconds,
	 * and can be overridden from the cloud.
	 * If the storage module is in passthrough mode, expiry of this timer will also trigger
	 * polling of the cloud shadow and FOTA status.
	 */
	TIMER_EXPIRED_SAMPLE_DATA,

	/* Timer for cloud synchronization has expired.
	 * This timer is used to trigger the cloud shadow and FOTA status polling and data sending.
	 * The timer is set to expire every CONFIG_APP_CLOUD_SYNC_INTERVAL_SECONDS seconds,
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
static void timer_send_data_work_fn(struct k_work *work);
static void timer_sample_data_work_fn(struct k_work *work);
static void timer_sample_start(uint32_t delay_sec);
static void timer_send_data_start(uint32_t delay_sec);
static void timer_sample_stop(void);
static void timer_send_data_stop(void);

/* Delayable work used to schedule triggers */
static K_WORK_DELAYABLE_DEFINE(timer_send_data_work, timer_send_data_work_fn);
static K_WORK_DELAYABLE_DEFINE(timer_sample_data_work, timer_sample_data_work_fn);

/* Forward declarations of state handlers */
static void running_entry(void *o);
static void running_run(void *o);

/* Storage mode handlers */
static void buffer_mode_entry(void *o);
static void buffer_mode_run(void *o);
static void buffer_mode_exit(void *o);
static void passthrough_mode_entry(void *o);
static void passthrough_mode_run(void *o);
static void passthrough_mode_exit(void *o);

/* Buffer mode connectivity handlers */
static void buffer_disconnected_entry(void *o);
static void buffer_disconnected_run(void *o);
static void buffer_connected_entry(void *o);
static void buffer_connected_run(void *o);

/* Buffer disconnected operation handlers */
static void buffer_disconnected_sampling_entry(void *o);
static void buffer_disconnected_sampling_run(void *o);
static void buffer_disconnected_waiting_entry(void *o);
static void buffer_disconnected_waiting_run(void *o);
static void buffer_disconnected_waiting_exit(void *o);

/* Buffer connected operation handlers */
static void buffer_connected_sampling_entry(void *o);
static void buffer_connected_sampling_run(void *o);
static void buffer_connected_waiting_entry(void *o);
static void buffer_connected_waiting_run(void *o);
static void buffer_connected_waiting_exit(void *o);

/* Passthrough mode connectivity handlers */
static void passthrough_disconnected_entry(void *o);
static void passthrough_disconnected_run(void *o);

/* Passthrough operation handlers */
static void passthrough_connected_sampling_entry(void *o);
static void passthrough_connected_sampling_run(void *o);
static void passthrough_connected_waiting_entry(void *o);
static void passthrough_connected_waiting_run(void *o);
static void passthrough_connected_waiting_exit(void *o);

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
	/* Main application is running */
	STATE_RUNNING,
		/* Application is in buffer storage mode */
		STATE_BUFFER_MODE,
			/* Buffer mode with no cloud connectivity */
			STATE_BUFFER_DISCONNECTED,
				/* Sampling sensor data and queuing cloud operations */
				STATE_BUFFER_DISCONNECTED_SAMPLING,
				/* Waiting for next sample trigger or user input */
				STATE_BUFFER_DISCONNECTED_WAITING,
			/* Buffer mode with active cloud connectivity */
			STATE_BUFFER_CONNECTED,
				/* Sampling sensor data and storing to buffer */
				STATE_BUFFER_CONNECTED_SAMPLING,
				/* Waiting for next sample or data send trigger */
				STATE_BUFFER_CONNECTED_WAITING,
		/* Application is in passthrough storage mode */
		STATE_PASSTHROUGH_MODE,
			/* Passthrough mode with no cloud connectivity */
			STATE_PASSTHROUGH_DISCONNECTED,
			/* Sampling sensors and sending data immediately */
			STATE_PASSTHROUGH_CONNECTED_SAMPLING,
			/* Waiting for next sample trigger or user input */
			STATE_PASSTHROUGH_CONNECTED_WAITING,

	/* Firmware Over-The-Air update is in progress */
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

	/* Start time of the most recent sampling. This is used to calculate the correct
	 * time when scheduling the next sampling trigger.
	 */
	uint32_t sample_start_time;

	/* Storage batch session ID for batch operations */
	uint32_t storage_session_id;

	/* Deep history of the last leaf state under STATE_RUNNING.
	 * Needed to transition to the correct state when coming back from FOTA.
	 */
	enum state running_history;
};

#if defined(CONFIG_APP_STORAGE_INITIAL_MODE_PASSTHROUGH)
#define INITIAL_MODE STATE_PASSTHROUGH_MODE
#else
#define INITIAL_MODE STATE_BUFFER_MODE
#endif

/* Construct state table */
static const struct smf_state states[] = {
	/* Top-level states */
	[STATE_RUNNING] = SMF_CREATE_STATE(
		running_entry,
		running_run,
		NULL,
		NULL,
		&states[INITIAL_MODE]
	),
	/* Storage mode states */
	[STATE_BUFFER_MODE] = SMF_CREATE_STATE(
		buffer_mode_entry,
		buffer_mode_run,
		buffer_mode_exit,
		&states[STATE_RUNNING],
		&states[STATE_BUFFER_DISCONNECTED]  /* Initially disconnected */
	),
	/* Buffer mode connectivity states */
	[STATE_BUFFER_DISCONNECTED] = SMF_CREATE_STATE(
		buffer_disconnected_entry,
		buffer_disconnected_run,
		NULL,
		&states[STATE_BUFFER_MODE],
		&states[STATE_BUFFER_DISCONNECTED_SAMPLING]
	),
	/* Buffer disconnected operation states */
	[STATE_BUFFER_DISCONNECTED_SAMPLING] = SMF_CREATE_STATE(
		buffer_disconnected_sampling_entry,
		buffer_disconnected_sampling_run,
		NULL,
		&states[STATE_BUFFER_DISCONNECTED],
		NULL
	),
	[STATE_BUFFER_DISCONNECTED_WAITING] = SMF_CREATE_STATE(
		buffer_disconnected_waiting_entry,
		buffer_disconnected_waiting_run,
		buffer_disconnected_waiting_exit,
		&states[STATE_BUFFER_DISCONNECTED],
		NULL
	),
	[STATE_BUFFER_CONNECTED] = SMF_CREATE_STATE(
		buffer_connected_entry,
		buffer_connected_run,
		NULL,
		&states[STATE_BUFFER_MODE],
		&states[STATE_BUFFER_CONNECTED_SAMPLING]
	),
	/* Buffer connected operation states */
	[STATE_BUFFER_CONNECTED_SAMPLING] = SMF_CREATE_STATE(
		buffer_connected_sampling_entry,
		buffer_connected_sampling_run,
		NULL,
		&states[STATE_BUFFER_CONNECTED],
		NULL
	),
	[STATE_BUFFER_CONNECTED_WAITING] = SMF_CREATE_STATE(
		buffer_connected_waiting_entry,
		buffer_connected_waiting_run,
		buffer_connected_waiting_exit,
		&states[STATE_BUFFER_CONNECTED],
		NULL
	),
	[STATE_PASSTHROUGH_MODE] = SMF_CREATE_STATE(
		passthrough_mode_entry,
		passthrough_mode_run,
		passthrough_mode_exit,
		&states[STATE_RUNNING],
		&states[STATE_PASSTHROUGH_DISCONNECTED]  /* Initially disconnected */
	),
	/* Passthrough mode connectivity states */
	[STATE_PASSTHROUGH_DISCONNECTED] = SMF_CREATE_STATE(
		passthrough_disconnected_entry,
		passthrough_disconnected_run,
		NULL,
		&states[STATE_PASSTHROUGH_MODE],
		NULL
	),
	/* Passthrough operation states */
	[STATE_PASSTHROUGH_CONNECTED_SAMPLING] = SMF_CREATE_STATE(
		passthrough_connected_sampling_entry,
		passthrough_connected_sampling_run,
		NULL,
		&states[STATE_PASSTHROUGH_MODE],
		NULL
	),
	[STATE_PASSTHROUGH_CONNECTED_WAITING] = SMF_CREATE_STATE(
		passthrough_connected_waiting_entry,
		passthrough_connected_waiting_run,
		passthrough_connected_waiting_exit,
		&states[STATE_PASSTHROUGH_MODE],
		NULL
	),
	/* FOTA states */
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

/* Helper functions shared across state handlers */

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

/* Common helpers for buffer-mode substates */

static void sampling_begin_common(struct main_state *state_object)
{
	int err;
	struct location_msg location_msg = {
		.type = LOCATION_SEARCH_TRIGGER,
	};
	uint32_t current_time = k_uptime_seconds();
	uint32_t time_elapsed = current_time - state_object->sample_start_time;

	if ((state_object->sample_start_time > 0) &&
	    (time_elapsed < state_object->sample_interval_sec)) {
		LOG_DBG("Too soon to start sampling, time_elapsed: %d, interval: %d",
			time_elapsed, state_object->sample_interval_sec);

		return;
	}

#if defined(CONFIG_APP_LED)
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

	state_object->sample_start_time = k_uptime_seconds();

	err = zbus_chan_pub(&LOCATION_CHAN, &location_msg,
			    K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to publish location search trigger, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void waiting_entry_common(const struct main_state *state_object)
{
	uint32_t time_elapsed;
	uint32_t time_remaining;

	time_elapsed = k_uptime_seconds() - state_object->sample_start_time;

	if (time_elapsed > state_object->sample_interval_sec) {
		LOG_WRN("Sampling took longer than the interval, time_elapsed: %d, interval: %d",
			time_elapsed, state_object->sample_interval_sec);
		time_remaining = 0;
	} else {
		time_remaining = state_object->sample_interval_sec - time_elapsed;
	}

	LOG_DBG("Next trigger in %d seconds", time_remaining);
	timer_sample_start(time_remaining);

#if defined(CONFIG_APP_LED)
	{
		int err;
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
			LOG_ERR("Failed to publish LED wait pattern, error: %d", err);
			SEND_FATAL_ERROR();
		}
	}
#endif /* CONFIG_APP_LED */
}

static void waiting_exit_common(void)
{
	timer_sample_stop();
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

/* Send stored data now, poll cloud/FOTA, and restart cloud send timer */
static void cloud_send_now(struct main_state *state_object)
{
	storage_send_data(state_object);
	poll_triggers_send();
	timer_send_data_start(state_object->data_send_interval_sec);
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

static void timer_sample_stop(void)
{
	int err;

	err = k_work_cancel_delayable(&timer_sample_data_work);
	if (err < 0) {
		LOG_ERR("k_work_cancel_delayable timer_sample_data_work, error: %d", err);
	}
}

static void timer_send_data_stop(void)
{
	int err;

	err = k_work_cancel_delayable(&timer_send_data_work);
	if (err < 0) {
		LOG_ERR("k_work_cancel_delayable timer_send_data_work, error: %d", err);
	}
}

/**
 * @brief Handle cloud shadow response
 *
 * Processes shadow updates to adjust sampling intervals based on
 * cloud configuration.
 */
static void handle_cloud_shadow_response(struct main_state *state_object,
					 const struct cloud_msg *msg)
{
	int err;
	uint32_t command_type = UINT32_MAX;
	uint32_t interval_sec = UINT32_MAX;

	err = get_parameters_from_cbor_response(
		msg->response.buffer,
		msg->response.buffer_data_len,
		&interval_sec,
		&command_type);
	if (err) {
		LOG_ERR("Failed to parse shadow response interval, error: %d", err);

		return;
	}

	/* Set new interval if valid */
	if (interval_sec != UINT32_MAX) {
		state_object->sample_interval_sec = interval_sec;

		LOG_DBG("Received new interval: %d seconds", state_object->sample_interval_sec);

		timer_sample_start(state_object->sample_interval_sec);
	}

	/* For commands, only process delta responses. This avoids executing commands that may still
	 * persist in the shadow's desired section. Delta responses only include new
	 * commands, since the device clears received commands by reporting them in
	 * the reported section of the shadow.
	 */
	if ((msg->type != CLOUD_SHADOW_RESPONSE_DELTA) && (command_type != UINT32_MAX)) {
		return;
	}

	LOG_DBG("Received command ID: %d, translated to %s", command_type,
		(command_type == CLOUD_COMMAND_TYPE_PROVISION) ?
		"CLOUD_COMMAND_TYPE_PROVISION" :
		(command_type == CLOUD_COMMAND_TYPE_REBOOT) ?
		"CLOUD_COMMAND_TYPE_REBOOT" : "UNKNOWN");

	/* Check for known commands */
	if (command_type == CLOUD_COMMAND_TYPE_PROVISION) {

		struct cloud_msg cloud_msg = {
			.type = CLOUD_PROVISIONING_REQUEST,
		};

		err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_SECONDS(1));
		if (err) {
			LOG_ERR("zbus_chan_pub, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}
	} else if (command_type == CLOUD_COMMAND_TYPE_REBOOT) {
		LOG_WRN("Received command ID: %d", command_type);

		/* Flush the log buffer and wait a few seconds before rebooting */
		LOG_PANIC();
		k_sleep(K_SECONDS(2));
		sys_reboot(SYS_REBOOT_COLD);
	}
}

/* Zephyr State Machine framework handlers */

/* STATE_RUNNING - Top level state */

static void running_entry(void *o)
{
	ARG_UNUSED(o);
	LOG_DBG("%s", __func__);
}

static void running_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	/* Handle FOTA download initiation at top level */
	if (state_object->chan == &FOTA_CHAN &&
	    MSG_TO_FOTA_TYPE(state_object->msg_buf) == FOTA_DOWNLOADING_UPDATE) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_FOTA]);

		return;
	}

	/* Handle storage mode change requests */
	if (state_object->chan == &STORAGE_CHAN) {
		const struct storage_msg *msg = MSG_TO_STORAGE_MSG(state_object->msg_buf);

		switch (msg->type) {
		case STORAGE_MODE_PASSTHROUGH:
			/* Storage module confirmed passthrough mode */
			smf_set_state(SMF_CTX(state_object), &states[STATE_PASSTHROUGH_MODE]);

			return;
		case STORAGE_MODE_BUFFER:
			/* Storage module confirmed buffer mode */
			smf_set_state(SMF_CTX(state_object), &states[STATE_BUFFER_MODE]);

			return;
		default:
			break;
		}
	}
}

/* STATE_BUFFER_MODE */

static void buffer_mode_entry(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	LOG_DBG("%s", __func__);

	/* Reset sample start time to allow immediate sampling when changing operation mode */
	state_object->sample_start_time = 0;

	timer_send_data_start(state_object->data_send_interval_sec);
}

static void buffer_mode_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	/* Handle mode change requests */
	if (state_object->chan == &STORAGE_CHAN) {
		const struct storage_msg *msg = MSG_TO_STORAGE_MSG(state_object->msg_buf);

		if (msg->type == STORAGE_MODE_PASSTHROUGH) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_PASSTHROUGH_MODE]);

			return;
		}
	}

	/* Handle long button press to send immediately */
	if (state_object->chan == &BUTTON_CHAN) {
		struct button_msg msg = MSG_TO_BUTTON_MSG(state_object->msg_buf);

		if (msg.type == BUTTON_PRESS_LONG) {
			/* Let buffer_connected_* handle long press when connected */
			smf_set_handled(SMF_CTX(state_object));
		}
	}
}

static void buffer_mode_exit(void *o)
{
	ARG_UNUSED(o);
	LOG_DBG("%s", __func__);

	timer_sample_stop();
	timer_send_data_stop();
}

/* STATE_BUFFER_DISCONNECTED */

static void buffer_disconnected_entry(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	LOG_DBG("%s", __func__);

	state_object->running_history = STATE_BUFFER_DISCONNECTED;
}

static void buffer_disconnected_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	/* Handle connectivity changes */
	if (state_object->chan == &CLOUD_CHAN) {
		const struct cloud_msg *msg = MSG_TO_CLOUD_MSG_PTR(state_object->msg_buf);

		if (msg->type == CLOUD_CONNECTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_BUFFER_CONNECTED]);

			return;
		}
	}

	/* Restart cloud send timer while disconnected */
	if (state_object->chan == &TIMER_CHAN &&
	    MSG_TO_TIMER_TYPE(state_object->msg_buf) == TIMER_EXPIRED_CLOUD) {
		timer_send_data_start(state_object->data_send_interval_sec);
		smf_set_handled(SMF_CTX(state_object));

		return;
	}
}

/* STATE_BUFFER_CONNECTED */

static void buffer_connected_entry(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	LOG_DBG("%s", __func__);

	state_object->running_history = STATE_BUFFER_CONNECTED;
}

static void buffer_connected_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	/* Handle connectivity changes */
	if (state_object->chan == &CLOUD_CHAN) {
		const struct cloud_msg *msg = MSG_TO_CLOUD_MSG_PTR(state_object->msg_buf);

		switch (msg->type) {
		case CLOUD_DISCONNECTED:
			smf_set_state(SMF_CTX(state_object), &states[STATE_BUFFER_DISCONNECTED]);

			return;
		case CLOUD_SHADOW_RESPONSE_DESIRED:
			__fallthrough;
		case CLOUD_SHADOW_RESPONSE_DELTA:
			handle_cloud_shadow_response(state_object, msg);

			break;
		default:
			break;
		}
	}

	/* Handle periodic send at connectivity parent level */
	if (state_object->chan == &TIMER_CHAN &&
	    MSG_TO_TIMER_TYPE(state_object->msg_buf) == TIMER_EXPIRED_CLOUD) {
		cloud_send_now(state_object);
		smf_set_handled(SMF_CTX(state_object));

		return;
	}

	/* Handle long button press to send immediately */
	if (state_object->chan == &BUTTON_CHAN) {
		struct button_msg button_msg = MSG_TO_BUTTON_MSG(state_object->msg_buf);

		if (button_msg.type == BUTTON_PRESS_LONG) {
			cloud_send_now(state_object);
			smf_set_handled(SMF_CTX(state_object));

			return;
		}
	}
}

/* STATE_BUFFER_DISCONNECTED_SAMPLING */

static void buffer_disconnected_sampling_entry(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	LOG_DBG("%s", __func__);
	sampling_begin_common(state_object);
}

static void buffer_disconnected_sampling_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	if (state_object->chan == &LOCATION_CHAN &&
	    MSG_TO_LOCATION_TYPE(state_object->msg_buf) == LOCATION_SEARCH_DONE) {

		/* In buffer mode when disconnected, just queue sensor sampling */
		sensor_triggers_send();
		smf_set_state(SMF_CTX(state_object), &states[STATE_BUFFER_DISCONNECTED_WAITING]);

		return;
	}

	/* Ignore other triggers while sampling */
	if (state_object->chan == &BUTTON_CHAN &&
	    MSG_TO_BUTTON_MSG(state_object->msg_buf).type == BUTTON_PRESS_SHORT) {
		smf_set_handled(SMF_CTX(state_object));

		return;
	}

	/* Handle cloud timer - queue data sending for when connected */
	/* TIMER_EXPIRED_CLOUD handled in parent buffer_disconnected_run */
}

/* STATE_BUFFER_DISCONNECTED_WAITING */

static void buffer_disconnected_waiting_entry(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	LOG_DBG("%s", __func__);
	waiting_entry_common(state_object);
}

static void buffer_disconnected_waiting_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	if (state_object->chan == &TIMER_CHAN &&
	    MSG_TO_TIMER_TYPE(state_object->msg_buf) == TIMER_EXPIRED_SAMPLE_DATA) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_BUFFER_DISCONNECTED_SAMPLING]);

		return;
	}

	if (state_object->chan == &BUTTON_CHAN &&
	    MSG_TO_BUTTON_MSG(state_object->msg_buf).type == BUTTON_PRESS_SHORT) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_BUFFER_DISCONNECTED_SAMPLING]);

		return;
	}

	/* Handle long button press - queue operations for when connected */
	if (state_object->chan == &BUTTON_CHAN &&
	    MSG_TO_BUTTON_MSG(state_object->msg_buf).type == BUTTON_PRESS_LONG) {
		LOG_DBG("Long button press while disconnected, operations will be queued");
		smf_set_handled(SMF_CTX(state_object));

		return;
	}
}

static void buffer_disconnected_waiting_exit(void *o)
{
	ARG_UNUSED(o);
	LOG_DBG("%s", __func__);

	waiting_exit_common();
}

/* STATE_BUFFER_CONNECTED_SAMPLING */

static void buffer_connected_sampling_entry(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	LOG_DBG("%s", __func__);
	sampling_begin_common(state_object);
}

static void buffer_connected_sampling_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	if (state_object->chan == &LOCATION_CHAN &&
	    MSG_TO_LOCATION_TYPE(state_object->msg_buf) == LOCATION_SEARCH_DONE) {

		/* In buffer mode when connected, sample all sensors */
		sensor_triggers_send();
		smf_set_state(SMF_CTX(state_object), &states[STATE_BUFFER_CONNECTED_WAITING]);

		return;
	}

	/* Ignore other triggers while sampling */
	if (state_object->chan == &BUTTON_CHAN &&
	    MSG_TO_BUTTON_MSG(state_object->msg_buf).type == BUTTON_PRESS_SHORT) {
		smf_set_handled(SMF_CTX(state_object));

		return;
	}

	/* Handle cloud timer */
	if (state_object->chan == &TIMER_CHAN &&
	    MSG_TO_TIMER_TYPE(state_object->msg_buf) == TIMER_EXPIRED_CLOUD) {
		timer_send_data_start(state_object->data_send_interval_sec);
		storage_send_data(state_object);
		poll_triggers_send();
		smf_set_handled(SMF_CTX(state_object));

		return;
	}
}

/* STATE_BUFFER_CONNECTED_WAITING */

static void buffer_connected_waiting_entry(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	LOG_DBG("%s", __func__);
	waiting_entry_common(state_object);
}

static void buffer_connected_waiting_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	if (state_object->chan == &TIMER_CHAN) {
		enum timer_msg_type timer_type = MSG_TO_TIMER_TYPE(state_object->msg_buf);

		if (timer_type == TIMER_EXPIRED_SAMPLE_DATA) {
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_BUFFER_CONNECTED_SAMPLING]);

			return;
		}

		if (timer_type == TIMER_EXPIRED_CLOUD) {
			storage_send_data(state_object);
			poll_triggers_send();
			timer_send_data_start(state_object->data_send_interval_sec);
			smf_set_handled(SMF_CTX(state_object));

			return;
		}
	}

	if (state_object->chan == &BUTTON_CHAN) {
		struct button_msg button_msg = MSG_TO_BUTTON_MSG(state_object->msg_buf);

		if (button_msg.type == BUTTON_PRESS_SHORT) {
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_BUFFER_CONNECTED_SAMPLING]);

			return;
		}

		if (button_msg.type == BUTTON_PRESS_LONG) {
			storage_send_data(state_object);
			poll_triggers_send();
			smf_set_handled(SMF_CTX(state_object));

			return;
		}
	}
}

static void buffer_connected_waiting_exit(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);
	waiting_exit_common();
}

/* STATE_PASSTHROUGH_MODE */

static void passthrough_mode_entry(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	LOG_DBG("%s", __func__);

	/* Reset sample start time to allow immediate sampling when changing operation mode */
	state_object->sample_start_time = 0;

	/* Cancel any existing timers - passthrough only operates when connected */
	timer_sample_stop();
	timer_send_data_stop();
}

static void passthrough_mode_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	/* Handle mode change requests */
	if (state_object->chan == &STORAGE_CHAN) {
		const struct storage_msg *msg = MSG_TO_STORAGE_MSG(state_object->msg_buf);

		if (msg->type == STORAGE_MODE_BUFFER) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_BUFFER_MODE]);

			return;
		}
	}

	/* Handle connectivity changes */
	if (state_object->chan == &CLOUD_CHAN) {
		const struct cloud_msg *msg = MSG_TO_CLOUD_MSG_PTR(state_object->msg_buf);

		switch (msg->type) {
		case CLOUD_DISCONNECTED:
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_PASSTHROUGH_DISCONNECTED]);
			return;
		case CLOUD_CONNECTED:
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_PASSTHROUGH_CONNECTED_SAMPLING]);
			return;
		case CLOUD_SHADOW_RESPONSE_DESIRED:
			__fallthrough;
		case CLOUD_SHADOW_RESPONSE_DELTA:
			handle_cloud_shadow_response(state_object, msg);

			return;
		default:
			break;
		}
	}
}

static void passthrough_mode_exit(void *o)
{
	ARG_UNUSED(o);
	LOG_DBG("%s", __func__);

	timer_sample_stop();
}

/* STATE_PASSTHROUGH_DISCONNECTED */

static void passthrough_disconnected_entry(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	LOG_DBG("%s", __func__);

	state_object->running_history = STATE_PASSTHROUGH_DISCONNECTED;
}

static void passthrough_disconnected_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	/* Handle connectivity changes */
	if (state_object->chan == &CLOUD_CHAN) {
		const struct cloud_msg *msg = MSG_TO_CLOUD_MSG_PTR(state_object->msg_buf);

		if (msg->type == CLOUD_CONNECTED) {
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_PASSTHROUGH_CONNECTED_SAMPLING]);
			return;
		}
	}
}

/* STATE_PASSTHROUGH_CONNECTED_SAMPLING */

static void passthrough_connected_sampling_entry(void *o)
{
	int err;
	struct location_msg location_msg = {
		.type = LOCATION_SEARCH_TRIGGER,
	};
	struct main_state *state_object = (struct main_state *)o;

	LOG_DBG("%s", __func__);

	/* Record history and the start time of sampling */
	state_object->running_history = STATE_PASSTHROUGH_CONNECTED_SAMPLING;
	state_object->sample_start_time = k_uptime_seconds();

	err = zbus_chan_pub(&LOCATION_CHAN, &location_msg, K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to publish location search trigger, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static void passthrough_connected_sampling_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	if (state_object->chan == &LOCATION_CHAN &&
	    MSG_TO_LOCATION_TYPE(state_object->msg_buf) == LOCATION_SEARCH_DONE) {

		/* In passthrough mode, send data immediately after sampling */
		sensor_triggers_send();
		poll_triggers_send();
		smf_set_state(SMF_CTX(state_object), &states[STATE_PASSTHROUGH_CONNECTED_WAITING]);

		return;
	}

	/* Ignore other triggers while sampling */
	if (state_object->chan == &BUTTON_CHAN &&
	    MSG_TO_BUTTON_MSG(state_object->msg_buf).type == BUTTON_PRESS_SHORT) {
		smf_set_handled(SMF_CTX(state_object));

		return;
	}
}

/* STATE_PASSTHROUGH_CONNECTED_WAITING */

static void passthrough_connected_waiting_entry(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;
	uint32_t time_elapsed = k_uptime_seconds() - state_object->sample_start_time;
	uint32_t time_remaining;

	LOG_DBG("%s", __func__);

	if (time_elapsed > state_object->sample_interval_sec) {
		time_remaining = 0;
	} else {
		time_remaining = state_object->sample_interval_sec - time_elapsed;
	}

	LOG_DBG("Passthrough mode: next trigger in %d seconds", time_remaining);
	timer_sample_start(time_remaining);
}

static void passthrough_connected_waiting_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	if (state_object->chan == &TIMER_CHAN &&
	    MSG_TO_TIMER_TYPE(state_object->msg_buf) == TIMER_EXPIRED_SAMPLE_DATA) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_PASSTHROUGH_CONNECTED_SAMPLING]);

		return;
	}

	if (state_object->chan == &BUTTON_CHAN &&
	    MSG_TO_BUTTON_MSG(state_object->msg_buf).type == BUTTON_PRESS_SHORT) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_PASSTHROUGH_CONNECTED_SAMPLING]);

		return;
	}

	/* Handle long button press - immediate send */
	if (state_object->chan == &BUTTON_CHAN &&
	    MSG_TO_BUTTON_MSG(state_object->msg_buf).type == BUTTON_PRESS_LONG) {
		LOG_DBG("Passthrough mode: long button press, immediate poll and send");
		poll_triggers_send();
		smf_set_handled(SMF_CTX(state_object));

		return;
	}
}

static void passthrough_connected_waiting_exit(void *o)
{
	ARG_UNUSED(o);
	LOG_DBG("%s", __func__);

	waiting_exit_common();
}

/* STATE_FOTA */

static void fota_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	/* Suspend all periodic activity while in FOTA */
	timer_sample_stop();
	timer_send_data_stop();
}

static void fota_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;
	const enum state resume_state = state_object->running_history;

	if (state_object->chan == &FOTA_CHAN) {
		enum fota_msg_type msg = MSG_TO_FOTA_TYPE(state_object->msg_buf);

		switch (msg) {
		case FOTA_DOWNLOAD_CANCELED:
			__fallthrough;
		case FOTA_DOWNLOAD_TIMED_OUT:
			__fallthrough;
		case FOTA_DOWNLOAD_FAILED:
			smf_set_state(SMF_CTX(state_object), &states[resume_state]);

			return;
		default:
			/* Don't care */
			break;
		}
	}

	/* Update cloud connection status to be able to return to the correct state in case
	 * cloud connection is lost during FOTA.
	 */
	if (state_object->chan == &CLOUD_CHAN) {
		const struct cloud_msg *msg = MSG_TO_CLOUD_MSG_PTR(state_object->msg_buf);

		if (msg->type == CLOUD_DISCONNECTED) {
			/* Figure out which state to return to in case FOTA is cancelled */
			switch (resume_state) {
			case STATE_PASSTHROUGH_CONNECTED_SAMPLING:
				state_object->running_history = STATE_PASSTHROUGH_DISCONNECTED;

				break;
			case STATE_BUFFER_CONNECTED:
				state_object->running_history = STATE_BUFFER_DISCONNECTED;

				break;
			case STATE_BUFFER_DISCONNECTED:
				/* No need to change state */
				break;
			default:
				break;
			}
		} else if (msg->type == CLOUD_CONNECTED) {
			switch (resume_state) {
			case STATE_BUFFER_DISCONNECTED:
				state_object->running_history = STATE_BUFFER_CONNECTED;

				break;
			case STATE_PASSTHROUGH_CONNECTED_SAMPLING:
				__fallthrough;
			case STATE_BUFFER_CONNECTED:
				/* No need to change state */
				break;
			default:
				break;
			}
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

	/* Ensure all timers are stopped while awaiting disconnect to apply image */
	timer_sample_stop();
	timer_send_data_stop();
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

	main_state.sample_interval_sec = CONFIG_APP_SENSOR_SAMPLING_INTERVAL_SECONDS;
	main_state.data_send_interval_sec = CONFIG_APP_CLOUD_SYNC_INTERVAL_SECONDS;

	LOG_DBG("Main has started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, task_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		SEND_FATAL_ERROR();

		return -EFAULT;
	}

	/* Set initial state - the hierarchy will automatically transition to correct mode */
	if (IS_ENABLED(CONFIG_APP_STORAGE_INITIAL_MODE_PASSTHROUGH)) {
		smf_set_initial(SMF_CTX(&main_state), &states[STATE_PASSTHROUGH_MODE]);
	} else {
		smf_set_initial(SMF_CTX(&main_state), &states[STATE_BUFFER_MODE]);
	}

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
