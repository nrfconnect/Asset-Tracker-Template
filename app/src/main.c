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
	 * The timer is set to expire every CONFIG_APP_SAMPLING_INTERVAL_SECONDS,
	 * and can be overridden from the cloud.
	 */
	TIMER_EXPIRED_SAMPLE_DATA,

	/* Timer for cloud synchronization has expired.
	 * This timer is used to trigger the cloud shadow and FOTA status polling and data sending.
	 * The timer is set to expire every CONFIG_APP_CLOUD_UPDATE_INTERVAL_SECONDS seconds,
	 * and can be overridden from the cloud.
	 */
	TIMER_EXPIRED_CLOUD,

	/* Configuration has changed, timers need to be restarted with new intervals.
	 * This internal event is used to signal that interval configuration has been updated
	 * and any active timers should be restarted to apply the new values.
	 */
	TIMER_CONFIG_CHANGED,
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
static enum smf_state_result running_run(void *o);
static void running_exit(void *o);

/* Connectivity handlers */
static void disconnected_entry(void *o);
static enum smf_state_result disconnected_run(void *o);
static void connected_entry(void *o);
static enum smf_state_result connected_run(void *o);

/* Disconnected operation handlers */
static void disconnected_sampling_entry(void *o);
static enum smf_state_result disconnected_sampling_run(void *o);
static void disconnected_waiting_entry(void *o);
static enum smf_state_result disconnected_waiting_run(void *o);
static void disconnected_waiting_exit(void *o);

/* Connected operation handlers */
static void connected_sampling_entry(void *o);
static enum smf_state_result connected_sampling_run(void *o);
static void connected_waiting_entry(void *o);
static enum smf_state_result connected_waiting_run(void *o);
static void connected_waiting_exit(void *o);
static void connected_sending_entry(void *o);
static enum smf_state_result connected_sending_run(void *o);

static void fota_entry(void *o);
static enum smf_state_result fota_run(void *o);

static void fota_downloading_entry(void *o);
static enum smf_state_result fota_downloading_run(void *o);

static void fota_waiting_for_network_disconnect_entry(void *o);
static enum smf_state_result fota_waiting_for_network_disconnect_run(void *o);

static void fota_waiting_for_network_disconnect_to_apply_image_entry(void *o);
static enum smf_state_result fota_waiting_for_network_disconnect_to_apply_image_run(void *o);

static void fota_applying_image_entry(void *o);
static enum smf_state_result fota_applying_image_run(void *o);

static void fota_rebooting_entry(void *o);

enum state {
	/* Main application is running */
	STATE_RUNNING,
		/* No cloud connectivity */
		STATE_DISCONNECTED,
			/* Sampling sensor data and queuing cloud operations */
			STATE_DISCONNECTED_SAMPLING,
			/* Waiting for next sample trigger or user input */
			STATE_DISCONNECTED_WAITING,
		/* Active cloud connectivity */
		STATE_CONNECTED,
			/* Sampling sensor data and storing to buffer */
			STATE_CONNECTED_SAMPLING,
			/* Waiting for next sample or data send trigger */
			STATE_CONNECTED_WAITING,
			/* Sending buffered data to the cloud */
			STATE_CONNECTED_SENDING,

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

	/* Update interval, how often the device synchronizes with the cloud */
	uint32_t update_interval_sec;

	uint32_t storage_threshold;

	/* Start time of the most recent sampling. This is used to calculate the correct
	 * time when scheduling the next sampling trigger.
	 */
	uint32_t sample_start_time;

	/* Start time of the most recent cloud sync. This is used to calculate the correct
	 * time when scheduling the next cloud sync trigger.
	 */
	uint32_t sync_start_time;

	/* Storage batch session ID for batch operations */
	uint32_t storage_session_id;

	/* Deep history of the last leaf state under STATE_RUNNING.
	 * Needed to transition to the correct state when coming back from FOTA.
	 */
	enum state running_history;

	/* Flag to track if shadow has been polled on initial connection */
	bool shadow_polled_on_connect;
};

/* Construct state table */
static const struct smf_state states[] = {
	/* Top-level states */
	[STATE_RUNNING] = SMF_CREATE_STATE(
		running_entry,
		running_run,
		running_exit,
		NULL,
		&states[STATE_DISCONNECTED]
	),
	/* Connectivity states */
	[STATE_DISCONNECTED] = SMF_CREATE_STATE(
		disconnected_entry,
		disconnected_run,
		NULL,
		&states[STATE_RUNNING],
		&states[STATE_DISCONNECTED_WAITING]
	),
	/* Disconnected operation states */
	[STATE_DISCONNECTED_SAMPLING] = SMF_CREATE_STATE(
		disconnected_sampling_entry,
		disconnected_sampling_run,
		NULL,
		&states[STATE_DISCONNECTED],
		NULL
	),
	[STATE_DISCONNECTED_WAITING] = SMF_CREATE_STATE(
		disconnected_waiting_entry,
		disconnected_waiting_run,
		disconnected_waiting_exit,
		&states[STATE_DISCONNECTED],
		NULL
	),
	[STATE_CONNECTED] = SMF_CREATE_STATE(
		connected_entry,
		connected_run,
		NULL,
		&states[STATE_RUNNING],
		&states[STATE_CONNECTED_WAITING]
	),
	/* Connected operation states */
	[STATE_CONNECTED_SAMPLING] = SMF_CREATE_STATE(
		connected_sampling_entry,
		connected_sampling_run,
		NULL,
		&states[STATE_CONNECTED],
		NULL
	),
	[STATE_CONNECTED_WAITING] = SMF_CREATE_STATE(
		connected_waiting_entry,
		connected_waiting_run,
		connected_waiting_exit,
		&states[STATE_CONNECTED],
		NULL
	),
	[STATE_CONNECTED_SENDING] = SMF_CREATE_STATE(
		connected_sending_entry,
		connected_sending_run,
		NULL,
		&states[STATE_CONNECTED],
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

static void poll_shadow_send(enum cloud_msg_type type)
{
	int err;
	struct cloud_msg cloud_msg = {
		.type = type
	};

	if ((type != CLOUD_SHADOW_GET_DESIRED) &&
	    (type != CLOUD_SHADOW_GET_DELTA)) {
		LOG_ERR("Invalid event: %d", type);
		return;
	}

	err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to publish cloud shadow poll trigger, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static void poll_triggers_send(void)
{
	int err;
	enum fota_msg_type fota_msg = FOTA_POLL_REQUEST;

	err = zbus_chan_pub(&FOTA_CHAN, &fota_msg, K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to publish FOTA poll trigger, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	/* Get the latest device configuration by polling the desired section of the shadow */
	poll_shadow_send(CLOUD_SHADOW_GET_DELTA);
}

/* Common helpers for substates */

static void sampling_begin_common(struct main_state *state_object,
				  const struct smf_state *fallback_state)
{
	int err;
	struct location_msg location_msg = {
		.type = LOCATION_SEARCH_TRIGGER,
	};
	uint32_t current_time = k_uptime_seconds();
	uint32_t time_elapsed = current_time - state_object->sample_start_time;

	if ((state_object->sample_start_time > 0) &&
	    (time_elapsed < state_object->sample_interval_sec) &&
	    (state_object->chan != &BUTTON_CHAN)) {
		LOG_DBG("Too soon to start sampling, time_elapsed: %d, interval: %d",
			time_elapsed, state_object->sample_interval_sec);

		/* Go back to waiting state to wait out the remainder of the interval */
		smf_set_state(SMF_CTX(state_object), fallback_state);

		return;
	}

#if defined(CONFIG_APP_LED)
	/* Blue pattern to indicate sampling */
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

		return;
	}
}

static void waiting_entry_common(const struct main_state *state_object)
{
	uint32_t time_elapsed;
	uint32_t time_remaining;

	/* Reschedule the next sample trigger */

	/* Special case: sample_start_time == 0 means first sample, trigger immediately */
	if (state_object->sample_start_time == 0) {
		time_remaining = 0;
	} else {
		time_elapsed = k_uptime_seconds() - state_object->sample_start_time;

		if (time_elapsed > state_object->sample_interval_sec) {
			LOG_WRN("Sampling took longer than the interval, time_elapsed: %d, interval: %d",
				time_elapsed, state_object->sample_interval_sec);
			time_remaining = 0;
		} else {
			time_remaining = state_object->sample_interval_sec - time_elapsed;
		}
	}

	LOG_DBG("Next sample trigger in %d seconds", time_remaining);
	timer_sample_start(time_remaining);

	/* Reschedule cloud sync trigger */
	time_elapsed = k_uptime_seconds() - state_object->sync_start_time;
	if (time_elapsed > state_object->update_interval_sec) {
		LOG_WRN("Cloud sync took longer than the update interval, time_elapsed: %d, interval: %d",
			time_elapsed, state_object->update_interval_sec);
		time_remaining = 0;
	} else {
		time_remaining = state_object->update_interval_sec - time_elapsed;
	}
	LOG_DBG("Next cloud sync trigger in %d seconds", time_remaining);
	timer_send_data_start(time_remaining);
}

static void waiting_exit_common(void)
{
	timer_sample_stop();
}

static void sensor_triggers_send(void)
{
	int err;

	(void)err;

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
	timer_send_data_start(state_object->update_interval_sec);
	state_object->sync_start_time = k_uptime_seconds();

#if defined(CONFIG_APP_LED)
	int err;
	/* Green pattern to indicate sending */
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

		return;
	}
}

static void timer_send_data_start(uint32_t delay_sec)
{
	int err;

	err = k_work_reschedule(&timer_send_data_work, K_SECONDS(delay_sec));
	if (err < 0) {
		LOG_ERR("k_work_reschedule timer_send_data_work, error: %d", err);
		SEND_FATAL_ERROR();

		return;
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

static void update_shadow_reported_section(const struct config_params *config,
					   uint32_t command_type,
					   uint32_t command_id)
{
	int err;
	struct cloud_msg cloud_msg = {
		.type = CLOUD_SHADOW_UPDATE_REPORTED,
	};
	size_t encoded_len;

	err = encode_shadow_parameters_to_cbor(config,
					       command_type,
					       command_id,
					       cloud_msg.payload.buffer,
					       sizeof(cloud_msg.payload.buffer),
					       &encoded_len);
	if (err) {
		LOG_ERR("encode_shadow_parameters_to_cbor, error: %d", err);
		return;
	}

	cloud_msg.payload.buffer_data_len = encoded_len;

	err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to publish config report, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	LOG_DBG("Configuration reported: update_interval=%d, sample_interval=%d, storage_threshold=%d",
		config->update_interval, config->sample_interval,
		config->storage_threshold);
}

static void config_apply(struct main_state *state_object, const struct config_params *config)
{
	int err;
	bool interval_changed = false;

	if (!config->sample_interval &&
	    !config->update_interval &&
	    !config->storage_threshold_valid) {
		LOG_DBG("No configuration parameters to update");
		return;
	}

	if (config->sample_interval &&
	    config->sample_interval != state_object->sample_interval_sec) {
		LOG_DBG("Updating sample interval to %d seconds", config->sample_interval);
		state_object->sample_interval_sec = config->sample_interval;
		interval_changed = true;
	}

	if (config->update_interval &&
	    config->update_interval != state_object->update_interval_sec) {
		LOG_DBG("Updating update interval to %d seconds", config->update_interval);
		state_object->update_interval_sec = config->update_interval;
		interval_changed = true;
	}

	if (config->storage_threshold_valid &&
	    config->storage_threshold != state_object->storage_threshold) {
		struct storage_msg storage_msg = {
			.type = STORAGE_SET_THRESHOLD,
			.data_len = config->storage_threshold,
		};

		LOG_DBG("Updating storage threshold to %d bytes", config->storage_threshold);
		state_object->storage_threshold = config->storage_threshold;

		err = zbus_chan_pub(&STORAGE_CHAN, &storage_msg, K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));
		if (err) {
			LOG_ERR("Failed to publish storage threshold update, error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}
	}

	/* Notify waiting states that configuration has changed and timers need restart */
	if (interval_changed) {
		enum timer_msg_type timer_msg = TIMER_CONFIG_CHANGED;

		/* Reset sample start time so re-entering waiting state uses full new interval */
		state_object->sample_start_time = k_uptime_seconds();

		err = zbus_chan_pub(&TIMER_CHAN, &timer_msg, K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));
		if (err) {
			LOG_ERR("Failed to publish timer config changed event, error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}
	}
}

static void command_execute(uint32_t command_type)
{
	if (command_type == CLOUD_COMMAND_TYPE_PROVISION) {
		LOG_DBG("Received provisioning command from cloud, requesting reprovisioning...");
		struct cloud_msg cloud_msg = {
			.type = CLOUD_PROVISIONING_REQUEST,
		};
		int err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));

		if (err) {
			LOG_ERR("zbus_chan_pub, error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}
	} else {
		LOG_DBG("No valid command to process");
	}
}

static void handle_cloud_shadow_response(struct main_state *state_object,
					 const struct cloud_msg *msg)
{
	int err;
	struct config_params config = { 0 };
	uint32_t command_type = 0;
	uint32_t command_id = 0;

	err = decode_shadow_parameters_from_cbor(msg->response.buffer,
						 msg->response.buffer_data_len,
						 &config,
						 &command_type,
						 &command_id);
	if (err) {
		LOG_ERR("Failed to parse shadow response, error: %d", err);
		/* Don't treat shadow configuration errors as fatal as they can occur if the
		 * format of the shadow changes.
		 */
		return;
	}

	config_apply(state_object, &config);

	/* Only report the configuration values that were actually applied by the application.
	 * Some parameters may be ignored or partially updated, so only the effective values
	 * should be reported back to the cloud.
	 */
	config.sample_interval = state_object->sample_interval_sec;
	config.update_interval = state_object->update_interval_sec;
	config.storage_threshold = state_object->storage_threshold;
	config.storage_threshold_valid = true;

	/* Only process commands from delta responses, not from desired responses.
	 * Delta responses contain only new commands that haven't been executed yet.
	 * While desired responses may contain old commands that have already been executed.
	 */
	if (msg->type == CLOUD_SHADOW_RESPONSE_DELTA) {
		/* Clear the shadow delta by reporting back the command to the cloud. */
		update_shadow_reported_section(&config, command_type, command_id);
		command_execute(command_type);
	} else {
		/* For desired responses (initial shadow poll), only report config without
		 * commands since we haven't executed any new commands.
		 */
		update_shadow_reported_section(&config, 0, 0);
	}
}

/* Zephyr State Machine framework handlers */

/* STATE_RUNNING - Top level state */

static void running_entry(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	ARG_UNUSED(o);
	LOG_DBG("%s", __func__);

	timer_send_data_start(state_object->update_interval_sec);
	state_object->sync_start_time = k_uptime_seconds();
}

static enum smf_state_result running_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	/* Handle FOTA download initiation at top level */
	if (state_object->chan == &FOTA_CHAN &&
	    MSG_TO_FOTA_TYPE(state_object->msg_buf) == FOTA_DOWNLOADING_UPDATE) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_FOTA]);

		return SMF_EVENT_HANDLED;
	}

	return SMF_EVENT_PROPAGATE;
}

static void running_exit(void *o)
{
	ARG_UNUSED(o);
	LOG_DBG("%s", __func__);

	timer_sample_stop();
	timer_send_data_stop();
}

/* STATE_DISCONNECTED */

static void disconnected_entry(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	LOG_DBG("%s", __func__);

	state_object->running_history = STATE_DISCONNECTED;
}

static enum smf_state_result disconnected_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	/* Handle connectivity changes */
	if (state_object->chan == &CLOUD_CHAN) {
		const struct cloud_msg *msg = MSG_TO_CLOUD_MSG_PTR(state_object->msg_buf);

		if (msg->type == CLOUD_CONNECTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED]);

			return SMF_EVENT_HANDLED;
		}
	}

	/* Restart cloud send timer while disconnected */
	if (state_object->chan == &TIMER_CHAN &&
	    MSG_TO_TIMER_TYPE(state_object->msg_buf) == TIMER_EXPIRED_CLOUD) {
		timer_send_data_start(state_object->update_interval_sec);
		state_object->sync_start_time = k_uptime_seconds();

		return SMF_EVENT_HANDLED;
	}

	/* Ignore send trigers when disconnected */
	if (state_object->chan == &BUTTON_CHAN) {
		struct button_msg button_msg = MSG_TO_BUTTON_MSG(state_object->msg_buf);

		if (button_msg.type == BUTTON_PRESS_LONG) {
			return SMF_EVENT_HANDLED;
		}
	}

	if (state_object->chan == &STORAGE_CHAN) {
		const struct storage_msg *msg = MSG_TO_STORAGE_MSG_PTR(state_object->msg_buf);

		if (msg->type == STORAGE_THRESHOLD_REACHED) {
			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* STATE_CONNECTED */

static void connected_entry(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	LOG_DBG("%s", __func__);

	state_object->running_history = STATE_CONNECTED;

	/* Get the latest device configuration by polling the desired section of the shadow. */
	if (!state_object->shadow_polled_on_connect) {
		poll_shadow_send(CLOUD_SHADOW_GET_DESIRED);
		state_object->shadow_polled_on_connect = true;
	}
}

static enum smf_state_result connected_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	/* Handle connectivity changes */
	if (state_object->chan == &CLOUD_CHAN) {
		const struct cloud_msg *msg = MSG_TO_CLOUD_MSG_PTR(state_object->msg_buf);

		switch (msg->type) {
		case CLOUD_DISCONNECTED:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED]);

			return SMF_EVENT_HANDLED;
		case CLOUD_SHADOW_RESPONSE_DESIRED:
			__fallthrough;
		case CLOUD_SHADOW_RESPONSE_DELTA:
			handle_cloud_shadow_response(state_object, msg);

			break;
		case CLOUD_SHADOW_RESPONSE_EMPTY_DESIRED:
			LOG_DBG("Received empty shadow response from cloud");

			struct config_params config = {
				.update_interval = state_object->update_interval_sec,
				.sample_interval = state_object->sample_interval_sec,
				.storage_threshold = state_object->storage_threshold,
			};

			update_shadow_reported_section(&config, 0, 0);

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	/* Handle periodic send at connectivity parent level */
	if (state_object->chan == &TIMER_CHAN &&
	    MSG_TO_TIMER_TYPE(state_object->msg_buf) == TIMER_EXPIRED_CLOUD) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_SENDING]);

		return SMF_EVENT_HANDLED;
	}

	/* Handle long button press to send immediately */
	if (state_object->chan == &BUTTON_CHAN) {
		struct button_msg button_msg = MSG_TO_BUTTON_MSG(state_object->msg_buf);

		if (button_msg.type == BUTTON_PRESS_LONG) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_SENDING]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* STATE_DISCONNECTED_SAMPLING */

static void disconnected_sampling_entry(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	LOG_DBG("%s", __func__);
	sampling_begin_common(state_object, &states[STATE_DISCONNECTED_WAITING]);
}

static enum smf_state_result disconnected_sampling_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	if (state_object->chan == &LOCATION_CHAN &&
	    MSG_TO_LOCATION_TYPE(state_object->msg_buf) == LOCATION_SEARCH_DONE) {

		sensor_triggers_send();
		smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_WAITING]);

		return SMF_EVENT_HANDLED;
	}

	/* Ignore other triggers while sampling */
	if (state_object->chan == &BUTTON_CHAN &&
	    MSG_TO_BUTTON_MSG(state_object->msg_buf).type == BUTTON_PRESS_SHORT) {
		return SMF_EVENT_HANDLED;
	}

	return SMF_EVENT_PROPAGATE;
}

/* STATE_DISCONNECTED_WAITING */

static void disconnected_waiting_entry(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	LOG_DBG("%s", __func__);
	waiting_entry_common(state_object);

#if defined(CONFIG_APP_LED)
	int err;
	/* Red pattern indicating disconnected */
	struct led_msg led_msg = {
		.type = LED_RGB_SET,
		.red = 55,
		.green = 0,
		.blue = 0,
		.duration_on_msec = 250,
		.duration_off_msec = 2000,
		.repetitions = 10,
	};

	err = zbus_chan_pub(&LED_CHAN, &led_msg, K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to publish LED pattern, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
#endif /* CONFIG_APP_LED */
}

static enum smf_state_result disconnected_waiting_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	if (state_object->chan == &TIMER_CHAN) {
		enum timer_msg_type timer_type = MSG_TO_TIMER_TYPE(state_object->msg_buf);

		if (timer_type == TIMER_EXPIRED_SAMPLE_DATA) {
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_DISCONNECTED_SAMPLING]);

			return SMF_EVENT_HANDLED;
		}

		if (timer_type == TIMER_CONFIG_CHANGED) {
			/* Re-enter state to restart timer with new interval */
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_DISCONNECTED_WAITING]);

			return SMF_EVENT_HANDLED;
		}
	}

	if (state_object->chan == &BUTTON_CHAN &&
	    MSG_TO_BUTTON_MSG(state_object->msg_buf).type == BUTTON_PRESS_SHORT) {
		smf_set_state(SMF_CTX(state_object),
			      &states[STATE_DISCONNECTED_SAMPLING]);

		return SMF_EVENT_HANDLED;
	}

	return SMF_EVENT_PROPAGATE;
}

static void disconnected_waiting_exit(void *o)
{
	ARG_UNUSED(o);
	LOG_DBG("%s", __func__);

	waiting_exit_common();
}

/* STATE_CONNECTED_SAMPLING */

static void connected_sampling_entry(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	LOG_DBG("%s", __func__);
	sampling_begin_common(state_object, &states[STATE_CONNECTED_WAITING]);
}

static enum smf_state_result connected_sampling_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	if (state_object->chan == &LOCATION_CHAN &&
	    MSG_TO_LOCATION_TYPE(state_object->msg_buf) == LOCATION_SEARCH_DONE) {

		sensor_triggers_send();

		if (SMF_CTX(state_object)->previous == &states[STATE_CONNECTED_SENDING]) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_SENDING]);
		} else {
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_WAITING]);
		}


		return SMF_EVENT_HANDLED;
	}

	/* Ignore other sample triggers while sampling */
	if (state_object->chan == &BUTTON_CHAN &&
	    MSG_TO_BUTTON_MSG(state_object->msg_buf).type == BUTTON_PRESS_SHORT) {
		return SMF_EVENT_HANDLED;
	}

	/* Handle buffer limit reached to send immediately */
	if (state_object->chan == &STORAGE_CHAN) {
		const struct storage_msg *msg = MSG_TO_STORAGE_MSG_PTR(state_object->msg_buf);

		if (msg->type == STORAGE_THRESHOLD_REACHED) {
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_CONNECTED_SENDING]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* STATE_CONNECTED_WAITING */

static void connected_waiting_entry(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	LOG_DBG("%s", __func__);
	waiting_entry_common(state_object);
}

static enum smf_state_result connected_waiting_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	if (state_object->chan == &TIMER_CHAN) {
		enum timer_msg_type timer_type = MSG_TO_TIMER_TYPE(state_object->msg_buf);

		if (timer_type == TIMER_EXPIRED_SAMPLE_DATA) {
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_CONNECTED_SAMPLING]);

			return SMF_EVENT_HANDLED;
		}

		if (timer_type == TIMER_CONFIG_CHANGED) {
			/* Re-enter state to restart timer with new interval */
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_CONNECTED_WAITING]);

			return SMF_EVENT_HANDLED;
		}
	}

	if (state_object->chan == &BUTTON_CHAN) {
		struct button_msg button_msg = MSG_TO_BUTTON_MSG(state_object->msg_buf);

		if (button_msg.type == BUTTON_PRESS_SHORT) {
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_CONNECTED_SAMPLING]);

			return SMF_EVENT_HANDLED;
		}
	}

	/* Handle buffer limit reached to send immediately */
	if (state_object->chan == &STORAGE_CHAN) {
		const struct storage_msg *msg = MSG_TO_STORAGE_MSG_PTR(state_object->msg_buf);

		if (msg->type == STORAGE_THRESHOLD_REACHED) {
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_CONNECTED_SENDING]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void connected_waiting_exit(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);
	waiting_exit_common();
}

static void connected_sending_entry(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	LOG_DBG("%s", __func__);

	/* Send data immediately when entering this state */
	cloud_send_now(state_object);
}

static enum smf_state_result connected_sending_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	if (state_object->chan == &TIMER_CHAN) {
		enum timer_msg_type timer_type = MSG_TO_TIMER_TYPE(state_object->msg_buf);

		if (timer_type == TIMER_EXPIRED_SAMPLE_DATA) {
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_CONNECTED_SAMPLING]);

			return SMF_EVENT_HANDLED;
		}

		/* Ignore cloud send timer while sending as we are already sending */
		if (timer_type == TIMER_EXPIRED_CLOUD) {
			return SMF_EVENT_HANDLED;
		}
	}

	if (state_object->chan == &BUTTON_CHAN) {
		struct button_msg button_msg = MSG_TO_BUTTON_MSG(state_object->msg_buf);

		if (button_msg.type == BUTTON_PRESS_SHORT) {
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_CONNECTED_SAMPLING]);

			return SMF_EVENT_HANDLED;
		}

		/* Ignore long press while sending as we are already sending */
		if (button_msg.type == BUTTON_PRESS_LONG) {
			return SMF_EVENT_HANDLED;
		}
	}

	if (state_object->chan == &STORAGE_CHAN) {
		const struct storage_msg *msg = MSG_TO_STORAGE_MSG_PTR(state_object->msg_buf);

		/* Ignore STORAGE_THRESHOLD_REACHED messages while sending */
		if (msg->type == STORAGE_THRESHOLD_REACHED) {
			return SMF_EVENT_HANDLED;
		}

		/* Storage batch closed indicates sending is done, go back to waiting */
		if (msg->type == STORAGE_BATCH_CLOSE) {
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_CONNECTED_WAITING]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* STATE_FOTA */

static void fota_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	/* Suspend all periodic activity while in FOTA */
	timer_sample_stop();
	timer_send_data_stop();

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

static enum smf_state_result fota_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;
	const enum state resume_state = state_object->running_history;

	if (state_object->chan == &FOTA_CHAN) {
		enum fota_msg_type msg = MSG_TO_FOTA_TYPE(state_object->msg_buf);

		switch (msg) {
		case FOTA_DOWNLOAD_CANCELED:
			__fallthrough;
		case FOTA_DOWNLOAD_REJECTED:
			__fallthrough;
		case FOTA_DOWNLOAD_TIMED_OUT:
			__fallthrough;
		case FOTA_DOWNLOAD_FAILED:
			smf_set_state(SMF_CTX(state_object), &states[resume_state]);

			return SMF_EVENT_HANDLED;
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
			case STATE_CONNECTED:
				state_object->running_history = STATE_DISCONNECTED;

				break;
			case STATE_DISCONNECTED:
				/* No need to change state */
				break;
			default:
				break;
			}
		} else if (msg->type == CLOUD_CONNECTED) {
			switch (resume_state) {
			case STATE_DISCONNECTED:
				state_object->running_history = STATE_CONNECTED;

				break;
			case STATE_CONNECTED:
				/* No need to change state */
				break;
			default:
				break;
			}
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* STATE_FOTA_DOWNLOADING */

static void fota_downloading_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);
}

static enum smf_state_result fota_downloading_run(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	if (state_object->chan == &FOTA_CHAN) {
		enum fota_msg_type msg = MSG_TO_FOTA_TYPE(state_object->msg_buf);

		switch (msg) {
		case FOTA_SUCCESS_REBOOT_NEEDED:
			smf_set_state(SMF_CTX(state_object),
					      &states[STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT]);

			return SMF_EVENT_HANDLED;
		case FOTA_IMAGE_APPLY_NEEDED:
			smf_set_state(SMF_CTX(state_object),
				&states[STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT_TO_APPLY_IMAGE]);

			return SMF_EVENT_HANDLED;
		default:
			/* Don't care */
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
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

		return;
	}
}

static enum smf_state_result fota_waiting_for_network_disconnect_run(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	if (state_object->chan == &NETWORK_CHAN) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		if (msg.type == NETWORK_DISCONNECTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_FOTA_REBOOTING]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
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

		return;
	}

	/* Ensure all timers are stopped while awaiting disconnect to apply image */
	timer_sample_stop();
	timer_send_data_stop();
}

static enum smf_state_result fota_waiting_for_network_disconnect_to_apply_image_run(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	if (state_object->chan == &NETWORK_CHAN) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		if (msg.type == NETWORK_DISCONNECTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_FOTA_APPLYING_IMAGE]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
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

		return;
	}
}

static enum smf_state_result fota_applying_image_run(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	if (state_object->chan == &FOTA_CHAN) {
		enum fota_msg_type msg = MSG_TO_FOTA_TYPE(state_object->msg_buf);

		if (msg == FOTA_SUCCESS_REBOOT_NEEDED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_FOTA_REBOOTING]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* STATE_FOTA_REBOOTING */

static void fota_rebooting_entry(void *o)
{
	ARG_UNUSED(o);

	struct storage_msg msg = { .type = STORAGE_CLEAR };
	int err;

	LOG_DBG("%s", __func__);

	/* Tell storage module to clear any stored data */
	err = zbus_chan_pub(&STORAGE_CHAN, &msg, K_MSEC(ZBUS_PUBLISH_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to publish storage clear message, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	/* Reboot the device */
	LOG_WRN("Rebooting the device to apply the FOTA update");

	/* Flush log buffer */
	LOG_PANIC();

	k_sleep(K_SECONDS(10));

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
	static struct main_state main_state;

	main_state.sample_interval_sec = CONFIG_APP_SAMPLING_INTERVAL_SECONDS;
	main_state.update_interval_sec = CONFIG_APP_CLOUD_UPDATE_INTERVAL_SECONDS;
	main_state.storage_threshold = CONFIG_APP_STORAGE_INITIAL_THRESHOLD;

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
