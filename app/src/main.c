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
#ifdef CONFIG_APP_INSPECT_SHELL
#include "app_inspect.h"
#endif /* CONFIG_APP_INSPECT_SHELL */
#include "network.h"
#include "cloud.h"
#include "fota.h"
#include "location.h"
#include "storage.h"
#include "cbor_helper.h"

#if defined(CONFIG_APP_BUTTON)
#include "button.h"
#endif /* CONFIG_APP_BUTTON */

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

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(main_subscriber);

enum timer_msg_type {
	/* Timer for sampling data has expired.
	 * This timer is used to trigger the sampling of data from the sensors.
	 * The timer is set to expire every CONFIG_APP_SAMPLING_INTERVAL_SECONDS,
	 * and can be overridden from the cloud.
	 */
	TIMER_EXPIRED_SAMPLE_DATA,

	/* Configuration has changed, timers need to be restarted with new intervals.
	 * This internal event is used to signal that interval configuration has been updated
	 * and any active timers should be restarted to apply the new values.
	 */
	TIMER_CONFIG_CHANGED,
};

struct timer_msg {
	enum timer_msg_type type;
};

ZBUS_CHAN_DEFINE(timer_chan,
		 struct timer_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

enum priv_main_msg_type {
	/* All modules have signaled that they are ready. */
	MAIN_MODULES_READY,

	/* The TN recovery interval has expired while connected over NTN. Time to leave NTN and
	 * attempt to return to a terrestrial network.
	 */
	MAIN_TN_RECOVERY_TIMEOUT,
};

struct priv_main_msg {
	enum priv_main_msg_type type;
};

ZBUS_CHAN_DEFINE(priv_main_chan,
		 struct priv_main_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Define the channels that the module subscribes to, their associated message types
 * and the subscriber that will receive the messages on the channel.
 * We use the X-macros to make the code more maintainable.
 */
#define CHANNEL_LIST(X)						\
	X(cloud_chan,		struct cloud_msg)		\
	X(fota_chan,		struct fota_msg)		\
	X(network_chan,		struct network_msg)		\
	X(location_chan,	struct location_msg)		\
	X(storage_chan,		struct storage_msg)		\
	X(timer_chan,		struct timer_msg)		\
	X(priv_main_chan,	struct priv_main_msg)		\
	IF_ENABLED(CONFIG_APP_BUTTON, (X(button_chan, struct button_msg)))	\
	IF_ENABLED(CONFIG_APP_POWER, (X(power_chan, struct power_msg)))

/* Calculate the maximum message size from the list of channels */
#define MAX_MSG_SIZE			MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

/* Add main_subscriber as observer to all the channels in the list. */
#define ADD_OBSERVERS(_chan, _type)	ZBUS_CHAN_ADD_OBS(_chan, main_subscriber, 0);

/*
 * Expand to a call to ZBUS_CHAN_ADD_OBS for each channel in the list.
 * Example: ZBUS_CHAN_ADD_OBS(cloud_chan, main_subscriber, 0);
 */
CHANNEL_LIST(ADD_OBSERVERS)

/* Forward declarations */
static void timer_sample_data_work_fn(struct k_work *work);
static void timer_sample_start(uint32_t delay_sec);
static void timer_sample_stop(void);

/* Delayable work used to schedule triggers */
static K_WORK_DELAYABLE_DEFINE(timer_sample_data_work, timer_sample_data_work_fn);

/* Forward declarations of state handlers */
static enum smf_state_result waiting_for_modules_init_run(void *o);
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

enum app_state {
	/* Waiting for module initialization */
	STATE_WAITING_FOR_MODULES_INIT,
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

	/* Storage threshold for triggering data send to cloud */
	uint32_t storage_threshold;

	/* Start time of the most recent sampling. This is used to calculate the correct
	 * time when scheduling the next sampling trigger.
	 */
	uint32_t sample_start_time;

	/* Used to fire the very first sample immediately on boot regardless
	 * of sample_start_time.
	 */
	bool first_sample_pending;

	/* Storage batch session ID for batch operations */
	uint32_t storage_session_id;

	/* Deep history of the last leaf state under STATE_RUNNING.
	 * Needed to transition to the correct state when coming back from FOTA.
	 */
	enum app_state running_history;

	/* Flag to track if cloud has been synced on initial connection
	 * Initial SHADOW_GET_DESIRED and FOTA_POLL_REQUEST
	 */
	bool cloud_synced_on_connect;

	/* Flag to track if the storage threshold was reached while disconnected.
	 * Used to decide whether to send data immediately on reconnection.
	 */
	bool threshold_reached;

	/* Track if NTN fallback is pending. This is used to prevent immediate switch to NTN if
	 * location search is in progress when TN search fails.
	 * The fallback to NTN will happen when the location search completes.
	 */
	bool ntn_fallback_pending;

	/* True while a cloud session (DTLS security context / connection ID) is alive, i.e. from
	 * CLOUD_SESSION_ESTABLISHED until CLOUD_SESSION_STOPPED. NTN fallback is only permitted to
	 * resume such a session via the connection ID; a fresh cloud connection is never attempted
	 * over NTN. Cleared means TN search failures retry TN instead of falling back to NTN.
	 */
	bool cloud_session_active;

	/* True while the network connection is over NTN. Used to arm the periodic TN recovery:
	 * NTN is a fallback bearer only, so the device must keep trying to return to TN.
	 */
	bool connected_via_ntn;

	/* Set when the TN recovery timer has fired and the network has been told to disconnect.
	 * When the resulting NETWORK_DISCONNECTED arrives, a TN search is started immediately
	 * instead of waiting for the reconnect back-off.
	 */
	bool tn_recovery_pending;

	/* Set when a sampling cycle started while connected over NTN, where GNSS (and cellular
	 * location) cannot run. The location sample is instead taken during the bearer bounce
	 * that follows: in place after reconnecting over TN, or - if TN is still unavailable -
	 * as the fresh GNSS fix forced into the NTN re-attach sequence.
	 */
	bool location_sample_pending;

	/* Periodic work that triggers a return-to-TN attempt while connected over NTN */
	struct k_work_delayable tn_recovery_work;

	/* Flags to track if each module is ready */
	struct {
		bool fota_ready;
#if defined(CONFIG_APP_POWER)
		bool power_ready;
#endif /* CONFIG_APP_POWER */
		bool location_ready;
	} modules_ready;

	/* True while waiting for GNSS fix requested by the network module */
	bool gnss_for_ntn_pending;

	/* Network reconnect back-off. After a failed connection attempt the TN->NTN cycle is
	 * restarted from TN, with the delay doubling on each consecutive failure up to
	 * CONFIG_APP_RECONNECT_BACKOFF_MAX_SECONDS. Reset on a successful connection.
	 */
	struct k_work_delayable reconnect_work;
	uint32_t reconnect_backoff_sec;
};

/* Construct state table */
static const struct smf_state states[] = {
	/* Initial state, waiting for modules to initialize */
	[STATE_WAITING_FOR_MODULES_INIT] = SMF_CREATE_STATE(
		NULL,
		waiting_for_modules_init_run,
		NULL,
		NULL,
		NULL
	),
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

#if defined(CONFIG_APP_INSPECT_SHELL)
static struct main_state *main_state_ctx;

static const char *main_state_to_string(enum app_state state)
{
	switch (state) {
	case STATE_WAITING_FOR_MODULES_INIT:
		return "STATE_WAITING_FOR_MODULES_INIT";
	case STATE_RUNNING:
		return "STATE_RUNNING";
	case STATE_DISCONNECTED:
		return "STATE_DISCONNECTED";
	case STATE_DISCONNECTED_SAMPLING:
		return "STATE_DISCONNECTED_SAMPLING";
	case STATE_DISCONNECTED_WAITING:
		return "STATE_DISCONNECTED_WAITING";
	case STATE_CONNECTED:
		return "STATE_CONNECTED";
	case STATE_CONNECTED_SAMPLING:
		return "STATE_CONNECTED_SAMPLING";
	case STATE_CONNECTED_WAITING:
		return "STATE_CONNECTED_WAITING";
	case STATE_CONNECTED_SENDING:
		return "STATE_CONNECTED_SENDING";
	case STATE_FOTA:
		return "STATE_FOTA";
	case STATE_FOTA_DOWNLOADING:
		return "STATE_FOTA_DOWNLOADING";
	case STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT:
		return "STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT";
	case STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT_TO_APPLY_IMAGE:
		return "STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT_TO_APPLY_IMAGE";
	case STATE_FOTA_APPLYING_IMAGE:
		return "STATE_FOTA_APPLYING_IMAGE";
	case STATE_FOTA_REBOOTING:
		return "STATE_FOTA_REBOOTING";
	default:
		return "STATE_UNKNOWN";
	}
}

APP_INSPECT_MODULE_REGISTER_STATE(main,
				  main_state_ctx,
				  states,
				  enum app_state,
				  main_state_to_string);
#endif /* CONFIG_APP_INSPECT_SHELL */

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
		SEND_FATAL_ERROR();

		return;
	}

	err = zbus_chan_pub(&cloud_chan, &cloud_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("Failed to publish cloud shadow poll trigger, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static void poll_triggers_send(void)
{
	int err;
	struct fota_msg fota_msg = { .type = FOTA_POLL_REQUEST };

	err = zbus_chan_pub(&fota_chan, &fota_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("Failed to publish FOTA poll trigger, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	/* Get the latest device configuration by polling the desired section of the shadow */
	poll_shadow_send(CLOUD_SHADOW_GET_DELTA);
}

/* Common helpers for substates */

/* Request a sample from all data sources. The location search is skipped when
 * include_location is false, i.e. while connected over NTN where neither GNSS nor cellular
 * location can run; the location sample is then taken during the bearer bounce instead.
 */
static void trigger_sampling_ex(struct main_state *state_object, bool include_location)
{
	int err;
	struct location_msg location_msg = {
		.type = LOCATION_SEARCH_TRIGGER,
	};

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

	err = zbus_chan_pub(&led_chan, &led_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("Failed to publish LED pattern message, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
#endif /* CONFIG_APP_LED */

	state_object->sample_start_time = k_uptime_seconds();
	state_object->first_sample_pending = false;

#if defined(CONFIG_APP_POWER)
	struct power_msg power_msg = {
		.type = POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST,
	};

	err = zbus_chan_pub(&power_chan, &power_msg, PUB_TIMEOUT);
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

	err = zbus_chan_pub(&environmental_chan, &environmental_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("Failed to publish environmental sensor sample request, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
#endif /* CONFIG_APP_ENVIRONMENTAL */

	if (!include_location) {
		return;
	}

	err = zbus_chan_pub(&location_chan, &location_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("Failed to publish location search trigger, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static void trigger_sampling(struct main_state *state_object)
{
	trigger_sampling_ex(state_object, true);
}

/* Request the network module to (re)connect over a terrestrial network. */
static void request_tn_connect(void)
{
	int err;
	const struct network_msg msg = { .type = NETWORK_CONNECT_TN };

	err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("Failed to publish NETWORK_CONNECT_TN, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

/* Request the network module to fall back to use NTN. With fresh_location set, the network module
 * acquires a new GNSS fix before the cell search even if its cached fix is still valid, so the
 * fix can double as a location sample.
 */
static void request_ntn_connect(bool fresh_location)
{
	int err;
	struct network_msg msg = {
		.type = NETWORK_CONNECT_NTN,
		.fresh_location = fresh_location,
	};

	err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("Failed to publish NETWORK_CONNECT_NTN, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

/* Start a return-to-TN bounce: disconnect from the current (NTN) bearer, which pauses the cloud
 * session, and flag the recovery so the TN search starts as soon as the network reports the
 * disconnect. If the TN search fails, the normal fallback resumes the session over NTN again.
 */
static void tn_recovery_start(struct main_state *state_object)
{
	int err;
	const struct network_msg msg = { .type = NETWORK_DISCONNECT };

	state_object->tn_recovery_pending = true;

	err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("Failed to publish NETWORK_DISCONNECT, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

/* Initial reconnect back-off. The delay doubles on each consecutive failure up to
 * CONFIG_APP_RECONNECT_BACKOFF_MAX_SECONDS.
 */
#define APP_RECONNECT_BACKOFF_INITIAL_SECONDS 60

static void reconnect_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_DBG("Reconnect back-off expired, restarting network search from TN");

	request_tn_connect();
}

/* Schedule the next reconnect attempt and grow the back-off for the following failure. */
static void reconnect_schedule(struct main_state *state_object)
{
	const uint32_t cap = CONFIG_APP_RECONNECT_BACKOFF_MAX_SECONDS;
	uint32_t delay_sec = MIN(state_object->reconnect_backoff_sec, cap);

	LOG_WRN("Connection attempt failed, restarting from TN in %u seconds", delay_sec);

	(void)k_work_reschedule(&state_object->reconnect_work, K_SECONDS(delay_sec));

	state_object->reconnect_backoff_sec = MIN(delay_sec * 2, cap);
}

/* Cancel any pending reconnect and reset the back-off after a successful connection. */
static void reconnect_reset(struct main_state *state_object)
{
	(void)k_work_cancel_delayable(&state_object->reconnect_work);
	state_object->reconnect_backoff_sec = APP_RECONNECT_BACKOFF_INITIAL_SECONDS;
}

/* TN recovery: NTN is a fallback bearer only, so while connected over NTN the application
 * periodically tries to return to TN. The timer fires on the system workqueue and defers the
 * actual work to the state machine thread via a private message, where it is acted on only if
 * the device is still connected over NTN.
 */
static void tn_recovery_work_fn(struct k_work *work)
{
	int err;
	const struct priv_main_msg msg = { .type = MAIN_TN_RECOVERY_TIMEOUT };

	ARG_UNUSED(work);

	err = zbus_chan_pub(&priv_main_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("Failed to publish MAIN_TN_RECOVERY_TIMEOUT, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void waiting_entry_common(const struct main_state *state_object)
{
	uint32_t time_elapsed;
	uint32_t time_remaining;

	/* Reschedule the next sample trigger */

	if (state_object->first_sample_pending) {
		time_remaining = 0;
	} else {
		time_elapsed = k_uptime_seconds() - state_object->sample_start_time;

		if (time_elapsed > state_object->sample_interval_sec) {
			LOG_WRN("Sampling took longer than the interval, time_elapsed: %d, "
				"interval: %d",
				time_elapsed, state_object->sample_interval_sec);
			time_remaining = 0;
		} else {
			time_remaining = state_object->sample_interval_sec - time_elapsed;
		}
	}

	LOG_DBG("Next sample trigger in %d seconds", time_remaining);

	timer_sample_start(time_remaining);
}

static void waiting_exit_common(void)
{
	timer_sample_stop();
}

static void storage_send_data(struct main_state *state_object)
{
	int err;
	struct storage_msg storage_msg = {
		.type = STORAGE_BATCH_REQUEST,
	};

	state_object->storage_session_id = k_uptime_get_32();
	storage_msg.session_id = state_object->storage_session_id;

	err = zbus_chan_pub(&storage_chan, &storage_msg, PUB_TIMEOUT);
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

	err = zbus_chan_pub(&led_chan, &led_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("Failed to publish LED pattern message, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
#endif /* CONFIG_APP_LED */
}

static void timer_sample_data_work_fn(struct k_work *work)
{
	int err;
	const struct timer_msg msg = { .type = TIMER_EXPIRED_SAMPLE_DATA };

	ARG_UNUSED(work);

	err = zbus_chan_pub(&timer_chan, &msg, PUB_TIMEOUT);
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

static void timer_sample_stop(void)
{
	int err;

	err = k_work_cancel_delayable(&timer_sample_data_work);
	if (err < 0) {
		LOG_ERR("k_work_cancel_delayable timer_sample_data_work, error: %d", err);
	}
}

static void update_shadow_reported_section(const struct config_params *config,
					   uint32_t command_type,
					   uint32_t command_id,
					   enum cloud_msg_type report_type)
{
	int err;
	struct cloud_msg cloud_msg = {
		.type = report_type,
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
		SEND_FATAL_ERROR();

		return;
	}

	cloud_msg.payload.buffer_data_len = encoded_len;

	err = zbus_chan_pub(&cloud_chan, &cloud_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("Failed to publish config report, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	if (config->sample_interval != 0) {
		LOG_DBG("Reported sample_interval: %d", config->sample_interval);
	}
	if (config->storage_threshold_valid) {
		LOG_DBG("Reported storage_threshold: %d", config->storage_threshold);
	}
}

static void config_apply(struct main_state *state_object, const struct config_params *config)
{
	int err;
	bool interval_changed = false;

	if (!config->sample_interval &&
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

	if (config->storage_threshold_valid &&
	    config->storage_threshold != state_object->storage_threshold) {
		struct storage_msg storage_msg = {
			.type = STORAGE_SET_THRESHOLD,
			.data_len = config->storage_threshold,
		};

		LOG_DBG("Updating storage threshold to %d samples", config->storage_threshold);
		state_object->storage_threshold = config->storage_threshold;

		err = zbus_chan_pub(&storage_chan, &storage_msg, PUB_TIMEOUT);
		if (err) {
			LOG_ERR("Failed to publish storage threshold update, error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}
	}

	/* Notify waiting states that configuration has changed and timers need restart */
	if (interval_changed) {
		const struct timer_msg timer_msg = { .type = TIMER_CONFIG_CHANGED };

		/* Reset sample start time so re-entering waiting state uses full new interval */
		state_object->sample_start_time = k_uptime_seconds();

		err = zbus_chan_pub(&timer_chan, &timer_msg, PUB_TIMEOUT);
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
		int err = zbus_chan_pub(&cloud_chan, &cloud_msg, PUB_TIMEOUT);

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
	struct config_params update_config = {0};
	struct config_params reported_config = {0};
	uint32_t command_type = 0;
	uint32_t command_id = 0;

	switch (msg->type) {
	/* For DELTA response, apply the changes from the delta, execute any commands, and report
	 * the updated configuration in the reported section.
	 * Only report the changed parameters in the reported section.
	 */
	case CLOUD_SHADOW_RESPONSE_DELTA:
		err = decode_shadow_parameters_from_cbor(
			msg->response.buffer, msg->response.buffer_data_len, &update_config,
			&command_type, &command_id);
		if (err) {
			LOG_ERR("Failed to parse shadow response, error: %d", err);
			/* Don't treat shadow configuration errors as fatal as they can occur if the
			 * format of the shadow changes.
			 */
			return;
		}

		config_apply(state_object, &update_config);

		reported_config.sample_interval =
			(update_config.sample_interval) ? state_object->sample_interval_sec : 0;
		reported_config.storage_threshold = (update_config.storage_threshold_valid)
							    ? state_object->storage_threshold
							    : 0;
		reported_config.storage_threshold_valid = update_config.storage_threshold_valid;

		update_shadow_reported_section(&reported_config, command_type, command_id,
					       CLOUD_SHADOW_UPDATE_REPORTED_CONFIG);

		command_execute(command_type);

		break;

	/* For DESIRED response, apply the configuration and report the full current configuration
	 * in the reported section.
	 */
	case CLOUD_SHADOW_RESPONSE_DESIRED:

		err = decode_shadow_parameters_from_cbor(
			msg->response.buffer, msg->response.buffer_data_len, &update_config,
			&command_type, &command_id);
		if (err) {
			LOG_ERR("Failed to parse shadow response, error: %d", err);
			/* Don't treat shadow configuration errors as fatal as they can occur if the
			 * format of the shadow changes.
			 */
			return;
		}

		config_apply(state_object, &update_config);

		reported_config.sample_interval = state_object->sample_interval_sec;
		reported_config.storage_threshold = state_object->storage_threshold;
		reported_config.storage_threshold_valid = true;

		update_shadow_reported_section(&reported_config, 0, 0,
					       CLOUD_SHADOW_SET_REPORTED_CONFIG);

		break;

	/* For EMPTY_DELTA response, do nothing */
	case CLOUD_SHADOW_RESPONSE_EMPTY_DELTA:
		LOG_DBG("Received empty shadow delta response, no configuration changes to apply");
		break;

	/* For EMPTY_DESIRED response, report the current configuration in the reported section. */
	case CLOUD_SHADOW_RESPONSE_EMPTY_DESIRED:

		reported_config.sample_interval = state_object->sample_interval_sec;
		reported_config.storage_threshold = state_object->storage_threshold;
		reported_config.storage_threshold_valid = true;

		update_shadow_reported_section(&reported_config, 0, 0,
					       CLOUD_SHADOW_SET_REPORTED_CONFIG);

		break;
	default:
		LOG_DBG("Received cloud message that is not a shadow response, ignoring: %d",
			msg->type);
		break;
	}
}

/* Check whether all modules that need time to initialize have reported ready.
 * If so, publish MAIN_MODULES_READY message to transition out of the waiting for modules state.
 */
static void check_modules_ready(const struct main_state *state_object)
{
	const struct priv_main_msg msg = { .type = MAIN_MODULES_READY };
	int err;

	if (state_object->modules_ready.fota_ready &&
#if defined(CONFIG_APP_POWER)
	    state_object->modules_ready.power_ready &&
#endif /* CONFIG_APP_POWER */
	    state_object->modules_ready.location_ready) {
		err = zbus_chan_pub(&priv_main_chan, &msg, PUB_TIMEOUT);
		if (err) {
			LOG_ERR("Failed to publish MAIN_MODULES_READY message, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}
	}
}

/* Zephyr State Machine framework handlers */

/* STATE_WAITING_FOR_MODULES_INIT */
static enum smf_state_result waiting_for_modules_init_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	/* Update the extended state per module, and check if all modules are ready. */
	if (state_object->chan == &fota_chan) {
		const struct fota_msg *msg = (const struct fota_msg *)state_object->msg_buf;

		if (msg->type == FOTA_MODULE_READY) {
			state_object->modules_ready.fota_ready = true;
			check_modules_ready(state_object);
			return SMF_EVENT_HANDLED;
		}
#if defined(CONFIG_APP_POWER)
	} else if (state_object->chan == &power_chan) {
		const struct power_msg *msg = (const struct power_msg *)state_object->msg_buf;

		if (msg->type == POWER_MODULE_READY) {
			state_object->modules_ready.power_ready = true;
			check_modules_ready(state_object);
			return SMF_EVENT_HANDLED;
		}
#endif /* CONFIG_APP_POWER */
	} else if (state_object->chan == &location_chan) {
		const struct location_msg *msg = (const struct location_msg *)state_object->msg_buf;

		if (msg->type == LOCATION_MODULE_READY) {
			state_object->modules_ready.location_ready = true;
			check_modules_ready(state_object);
			return SMF_EVENT_HANDLED;
		}

	/* If all modules are ready, we can transition to the running state. */
	} else if (state_object->chan == &priv_main_chan) {
		const struct priv_main_msg *msg =
			(const struct priv_main_msg *)state_object->msg_buf;

		if (msg->type == MAIN_MODULES_READY) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_RUNNING]);
			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void running_entry(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	LOG_DBG("%s", __func__);

	state_object->gnss_for_ntn_pending = false;

	request_tn_connect();
}

static enum smf_state_result handle_running_network_msg(struct main_state *state_object,
							const struct network_msg *msg)
{
	int err;

	switch (msg->type) {
	case NETWORK_TN_LIGHT_SEARCH_DONE:
		LOG_WRN("Light search on TN done, no suitable cell");

		if (IS_ENABLED(CONFIG_APP_NTN_FALLBACK_ON_LIGHT_SEARCH_DONE)) {
			request_ntn_connect(state_object->location_sample_pending);

			return SMF_EVENT_HANDLED;
		}

		break;
	case NETWORK_TN_SEARCH_FAILED:
		/* Only fall back to NTN to resume an existing cloud connection or if the app is
		 * configured to allow cloud connection establishment over NTN.
		 */
		if (state_object->cloud_session_active ||
		    IS_ENABLED(CONFIG_APP_NTN_CLOUD_ALLOW_CONNECTION_ESTABLISHMENT)) {
			request_ntn_connect(state_object->location_sample_pending);
		} else {
			LOG_INF("No active cloud session to resume, not falling back to NTN");
			reconnect_schedule(state_object);
		}

		return SMF_EVENT_HANDLED;
	case NETWORK_NTN_LIGHT_SEARCH_DONE:
		LOG_WRN("Light search on NTN done, no suitable cell");

		break;
	case NETWORK_NTN_SEARCH_FAILED:
		/* The TN->NTN connection attempt is exhausted. Retry from TN after a back-off so
		 * the device can recover (e.g. when no GNSS fix is available for NTN).
		 */
		reconnect_schedule(state_object);

		return SMF_EVENT_HANDLED;
	case NETWORK_CONNECTED_TN:
		reconnect_reset(state_object);

		state_object->connected_via_ntn = false;

		/* Back on TN; no recovery needed. */
		(void)k_work_cancel_delayable(&state_object->tn_recovery_work);

		if (state_object->location_sample_pending) {
			/* A sampling cycle was interrupted by the bearer bounce. GNSS works on TN,
			 * so take the location sample in place now.
			 */
			struct location_msg loc_trigger = { .type = LOCATION_SEARCH_TRIGGER };

			state_object->location_sample_pending = false;

			err = zbus_chan_pub(&location_chan, &loc_trigger, PUB_TIMEOUT);
			if (err) {
				LOG_ERR("Failed to publish LOCATION_SEARCH_TRIGGER, error: %d", err);
				SEND_FATAL_ERROR();
			}
		}

		return SMF_EVENT_HANDLED;
	case NETWORK_CONNECTED_NTN:
		reconnect_reset(state_object);
		state_object->connected_via_ntn = true;

		/* NTN is a fallback bearer only: it is slow, expensive, and blocks GNSS and
		 * cellular location while active. Periodically try to return to TN.
		 * k_work_schedule() keeps an already-running interval instead of pushing it out
		 * when the connected event is re-asserted.
		 */
		(void)k_work_schedule(
			&state_object->tn_recovery_work,
			K_SECONDS(CONFIG_APP_TN_RECOVERY_INTERVAL_SECONDS));

		return SMF_EVENT_HANDLED;
	case NETWORK_DISCONNECTED:
		state_object->connected_via_ntn = false;

		if (state_object->tn_recovery_pending) {
			/* This disconnect was requested by the TN recovery timer. Start the TN
			 * search right away; if it fails, the normal fallback resumes the session
			 * over NTN again.
			 */
			state_object->tn_recovery_pending = false;

			LOG_DBG("TN recovery: disconnected from NTN, searching for TN");
			request_tn_connect();

			return SMF_EVENT_HANDLED;
		}

		return SMF_EVENT_PROPAGATE;
	case NETWORK_GNSS_LOCATION_REQ: {
		struct location_msg loc_msg = { .type = LOCATION_GNSS_SEARCH_TRIGGER };

		state_object->gnss_for_ntn_pending = true;

		err = zbus_chan_pub(&location_chan, &loc_msg, PUB_TIMEOUT);
		if (err) {
			LOG_ERR("Failed to publish LOCATION_GNSS_SEARCH_TRIGGER, error: %d", err);
			SEND_FATAL_ERROR();
		}

		return SMF_EVENT_HANDLED;
	}
	default:
		return SMF_EVENT_PROPAGATE;
	}

	return SMF_EVENT_PROPAGATE;
}

/* Relay a GNSS fix requested by the network module (for the NTN attach) back to it. Only reached
 * while gnss_for_ntn_pending is set.
 */
static enum smf_state_result handle_running_location_msg(struct main_state *state_object,
							 const struct location_msg *msg)
{
	struct network_msg nw_msg;
	int err;

	if (msg->type == LOCATION_GNSS_DATA) {
		state_object->gnss_for_ntn_pending = false;

		/* The location module has published this fix, so it has also been stored as a
		 * location sample; a sample deferred by an NTN bounce is now taken.
		 */
		state_object->location_sample_pending = false;

		nw_msg.type = NETWORK_GNSS_LOCATION;
		nw_msg.location.lat = msg->gnss_data.latitude;
		nw_msg.location.lon = msg->gnss_data.longitude;
		nw_msg.location.alt = msg->gnss_data.details.gnss.pvt_data.altitude;

		err = zbus_chan_pub(&network_chan, &nw_msg, PUB_TIMEOUT);
		if (err) {
			LOG_ERR("Failed to publish NETWORK_GNSS_LOCATION, error: %d", err);
			SEND_FATAL_ERROR();
		}

		return SMF_EVENT_HANDLED;
	}

	if (msg->type == LOCATION_SEARCH_DONE) {
		state_object->gnss_for_ntn_pending = false;
		nw_msg.type = NETWORK_GNSS_LOCATION_FAILED;

		err = zbus_chan_pub(&network_chan, &nw_msg, PUB_TIMEOUT);
		if (err) {
			LOG_ERR("Failed to publish NETWORK_GNSS_LOCATION_FAILED, error: %d", err);
			SEND_FATAL_ERROR();
		}

		return SMF_EVENT_HANDLED;
	}

	return SMF_EVENT_PROPAGATE;
}

static enum smf_state_result handle_running_fota_msg(struct main_state *state_object,
						     const struct fota_msg *msg)
{
	if (msg->type == FOTA_DOWNLOADING_UPDATE) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_FOTA]);

		return SMF_EVENT_HANDLED;
	}

	return SMF_EVENT_PROPAGATE;
}

static enum smf_state_result handle_running_cloud_msg(struct main_state *state_object,
						      const struct cloud_msg *msg)
{
	switch (msg->type) {
	case CLOUD_PROVISIONED:
		LOG_DBG("Device provisioning completed");

		/* After reprovisioning, the device shadow is no longer considered synced with the
		 * cloud, so reset the flag to trigger a new sync on the next connection.
		 */
		state_object->cloud_synced_on_connect = false;

		return SMF_EVENT_HANDLED;
	case CLOUD_SESSION_ESTABLISHED:
		/* A cloud session is now alive. NTN fallback is permitted to resume it. */
		state_object->cloud_session_active = true;

		return SMF_EVENT_HANDLED;
	case CLOUD_SESSION_STOPPED:
		/* The cloud session is gone. NTN fallback is no longer permitted; a new connection
		 * must be (re)established over TN.
		 */
		state_object->cloud_session_active = false;

		return SMF_EVENT_HANDLED;
	default:
		return SMF_EVENT_PROPAGATE;
	}
}

static enum smf_state_result running_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	if (state_object->chan == &network_chan) {
		return handle_running_network_msg(
			state_object, (const struct network_msg *)state_object->msg_buf);
	}

	if (state_object->chan == &location_chan && state_object->gnss_for_ntn_pending) {
		return handle_running_location_msg(
			state_object, (const struct location_msg *)state_object->msg_buf);
	}

	if (state_object->chan == &fota_chan) {
		return handle_running_fota_msg(
			state_object, (const struct fota_msg *)state_object->msg_buf);
	}

	if (state_object->chan == &cloud_chan) {
		return handle_running_cloud_msg(
			state_object, (const struct cloud_msg *)state_object->msg_buf);
	}

	return SMF_EVENT_PROPAGATE;
}

static void running_exit(void *o)
{
	ARG_UNUSED(o);
	LOG_DBG("%s", __func__);

	timer_sample_stop();
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
	if (state_object->chan == &cloud_chan) {
		const struct cloud_msg *msg = (const struct cloud_msg *)state_object->msg_buf;

		if (msg->type == CLOUD_CONNECTED) {
			if (state_object->threshold_reached) {
				state_object->threshold_reached = false;
				smf_set_state(SMF_CTX(state_object),
					      &states[STATE_CONNECTED_SENDING]);
			} else {
				smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED]);
			}

			return SMF_EVENT_HANDLED;
		}
	}

#if defined(CONFIG_APP_BUTTON)
	/* Ignore send trigers when disconnected */
	if (state_object->chan == &button_chan) {
		const struct button_msg *msg = (const struct button_msg *)state_object->msg_buf;

		if (msg->type == BUTTON_PRESS_LONG) {
			return SMF_EVENT_HANDLED;
		}
	}
#endif /* CONFIG_APP_BUTTON */

	if (state_object->chan == &storage_chan) {
		const struct storage_msg *msg = (const struct storage_msg *)state_object->msg_buf;

		if (msg->type == STORAGE_THRESHOLD_REACHED) {
			state_object->threshold_reached = true;
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

	/* On initial connection, update shadow reported info, and poll shadow desired and FOTA
	 * status. Ensures synced states between device and cloud.
	 */
	if (!state_object->cloud_synced_on_connect) {

		int err;
		struct fota_msg fota_msg = { .type = FOTA_POLL_REQUEST };
		struct cloud_msg cloud_msg = {
			.type = CLOUD_SHADOW_UPDATE_REPORTED_DEVICE
		};

		err = zbus_chan_pub(&cloud_chan, &cloud_msg, PUB_TIMEOUT);
		if (err) {
			LOG_ERR("Failed to publish cloud shadow poll trigger, error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}

		err = zbus_chan_pub(&fota_chan, &fota_msg, PUB_TIMEOUT);
		if (err) {
			LOG_ERR("Failed to trigger FOTA polling on cloud connection: %d", err);
		}

		poll_shadow_send(CLOUD_SHADOW_GET_DESIRED);
		state_object->cloud_synced_on_connect = true;
	}
}

static enum smf_state_result connected_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	/* TN recovery: while connected over NTN, the recovery timer periodically requests a
	 * return to TN. Disconnect (which pauses the cloud session, preserving it) and flag the
	 * recovery so the TN search starts as soon as the network reports the disconnect. Acted
	 * on here, in the connected state only, so a timeout firing during FOTA or after the NTN
	 * link already dropped is ignored.
	 */
	if (state_object->chan == &priv_main_chan) {
		const struct priv_main_msg *msg =
			(const struct priv_main_msg *)state_object->msg_buf;

		if (msg->type == MAIN_TN_RECOVERY_TIMEOUT) {
			if (state_object->connected_via_ntn) {
				LOG_INF("TN recovery: leaving NTN to search for terrestrial network");
				tn_recovery_start(state_object);
			}

			return SMF_EVENT_HANDLED;
		}
	}

	/* While connected, ignore search-failed events instead of letting them propagate to the
	 * top-level handler that would start a fallback search. A search timeout work item can be
	 * delivered just after the connection comes up (timer cancellation in the network module
	 * is best-effort), and acting on it would tear down a healthy link.
	 */
	if (state_object->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		if (msg->type == NETWORK_TN_SEARCH_FAILED ||
		    msg->type == NETWORK_NTN_SEARCH_FAILED) {
			return SMF_EVENT_HANDLED;
		}
	}

	/* Handle connectivity changes */
	if (state_object->chan == &cloud_chan) {
		const struct cloud_msg *msg = (const struct cloud_msg *)state_object->msg_buf;

		switch (msg->type) {
		case CLOUD_DISCONNECTED:
			/* The cloud link dropped (the cloud module either paused a still-alive
			 * session or tore it down). Drive the connection lifecycle back up: schedule a
			 * TN search after the back-off. If TN returns, the (possibly paused) session
			 * resumes over it; if the TN search fails and a cloud session is still alive,
			 * the top-level handler falls back to NTN to resume it.
			 */
			reconnect_schedule(state_object);
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED]);

			return SMF_EVENT_HANDLED;
		case CLOUD_SHADOW_RESPONSE_DESIRED:
			__fallthrough;
		case CLOUD_SHADOW_RESPONSE_DELTA:
			 __fallthrough;
		case CLOUD_SHADOW_RESPONSE_EMPTY_DELTA:
			 __fallthrough;
		case CLOUD_SHADOW_RESPONSE_EMPTY_DESIRED:
			handle_cloud_shadow_response(state_object, msg);

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

#if defined(CONFIG_APP_BUTTON)
	/* Handle long button press to send immediately */
	if (state_object->chan == &button_chan) {
		const struct button_msg *msg = (const struct button_msg *)state_object->msg_buf;

		if (msg->type == BUTTON_PRESS_LONG) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_SENDING]);

			return SMF_EVENT_HANDLED;
		}
	}
#endif /* CONFIG_APP_BUTTON */

	/* Handle buffer limit reached to send immediately */
	if (state_object->chan == &storage_chan) {
		const struct storage_msg *msg = (const struct storage_msg *)state_object->msg_buf;

		if (msg->type == STORAGE_THRESHOLD_REACHED) {
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

	/* Start each sampling cycle with a clean NTN fallback state. This prevents a deferred
	 * fallback left over from an earlier cycle that was aborted.
	 */
	state_object->ntn_fallback_pending = false;

	trigger_sampling(state_object);
}

static enum smf_state_result disconnected_sampling_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	if (state_object->chan == &location_chan) {
		const struct location_msg *msg = (const struct location_msg *)state_object->msg_buf;

		if (msg->type == LOCATION_SEARCH_DONE) {
			if (state_object->ntn_fallback_pending) {
				state_object->ntn_fallback_pending = false;

				/* The location search that just completed already produced (and
				 * stored) a fix, so the NTN attach can use the cached one.
				 */
				request_ntn_connect(false);
			}

			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_WAITING]);

			return SMF_EVENT_HANDLED;
		}
	}

	if (state_object->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		/* A location search is already in progress, so do not start the NTN search
		 * immediately. Starting it now would switch the modem system mode and could request
		 * a GNSS fix, conflicting with the ongoing search. Defer the fallback until the
		 * search completes (handled on LOCATION_SEARCH_DONE above).
		 */
		if (msg->type == NETWORK_TN_SEARCH_FAILED) {
			/* Only defer an NTN fallback when there is a paused cloud session to resume.
			 * Without one, NTN cannot be used (a fresh connection over NTN is not
			 * permitted), so retry TN instead.
			 */
			if (state_object->cloud_session_active) {
				state_object->ntn_fallback_pending = true;

				LOG_DBG("TN search failed, NTN fallback pending");
			} else {
				LOG_INF("No active cloud session to resume; "
					"not falling back to NTN, retrying TN");
				reconnect_schedule(state_object);
			}

			return SMF_EVENT_HANDLED;
		}
	}

#if defined(CONFIG_APP_BUTTON)
	/* Ignore other triggers while sampling */
	if (state_object->chan == &button_chan) {
		const struct button_msg *msg = (const struct button_msg *)state_object->msg_buf;

		if (msg->type == BUTTON_PRESS_SHORT) {
			return SMF_EVENT_HANDLED;
		}
	}
#endif /* CONFIG_APP_BUTTON */

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

	err = zbus_chan_pub(&led_chan, &led_msg, PUB_TIMEOUT);
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

	if (state_object->chan == &timer_chan) {
		const struct timer_msg *msg = (const struct timer_msg *)state_object->msg_buf;

		if (msg->type == TIMER_EXPIRED_SAMPLE_DATA) {
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_DISCONNECTED_SAMPLING]);

			return SMF_EVENT_HANDLED;
		}

		if (msg->type == TIMER_CONFIG_CHANGED) {
			/* Re-enter state to restart timer with new interval */
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_DISCONNECTED_WAITING]);

			return SMF_EVENT_HANDLED;
		}
	}

#if defined(CONFIG_APP_BUTTON)
	if (state_object->chan == &button_chan) {
		const struct button_msg *msg = (const struct button_msg *)state_object->msg_buf;

		if (msg->type == BUTTON_PRESS_SHORT) {
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_DISCONNECTED_SAMPLING]);

			return SMF_EVENT_HANDLED;
		}
	}
#endif /* CONFIG_APP_BUTTON */

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

	if (state_object->connected_via_ntn) {
		/* GNSS and cellular location cannot run while the modem is in NTN system mode.
		 * Sample the non-location sources in place, then bounce the bearer: disconnect
		 * (pausing the cloud session) and search TN first. If TN is back, the location
		 * sample is taken there; if not, the NTN re-attach is forced to acquire a fresh
		 * GNSS fix which doubles as the sample. The cloud disconnect that follows moves
		 * this state machine out of the sampling state.
		 */
		LOG_INF("Sampling on NTN: bouncing bearer to take the location sample");

		trigger_sampling_ex(state_object, false);

		state_object->location_sample_pending = true;

		tn_recovery_start(state_object);

		return;
	}

	trigger_sampling(state_object);
}

static enum smf_state_result connected_sampling_run(void *o)
{
	struct main_state *state_object = (struct main_state *)o;

	if (state_object->chan == &location_chan) {
		const struct location_msg *msg = (const struct location_msg *)state_object->msg_buf;

		if (msg->type == LOCATION_SEARCH_DONE) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_WAITING]);
			return SMF_EVENT_HANDLED;
		}
	}

#if defined(CONFIG_APP_BUTTON)
	/* Ignore other sample triggers while sampling */
	if (state_object->chan == &button_chan) {
		const struct button_msg *msg = (const struct button_msg *)state_object->msg_buf;

		if (msg->type == BUTTON_PRESS_SHORT) {
			return SMF_EVENT_HANDLED;
		}
	}
#endif /* CONFIG_APP_BUTTON */

	if (state_object->chan == &storage_chan) {
		const struct storage_msg *msg = (const struct storage_msg *)state_object->msg_buf;

		if (msg->type == STORAGE_THRESHOLD_REACHED) {
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

	if (state_object->chan == &timer_chan) {
		const struct timer_msg *msg = (const struct timer_msg *)state_object->msg_buf;

		if (msg->type == TIMER_EXPIRED_SAMPLE_DATA) {
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_CONNECTED_SAMPLING]);

			return SMF_EVENT_HANDLED;
		}

		if (msg->type == TIMER_CONFIG_CHANGED) {
			/* Re-enter state to restart timer with new interval */
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_CONNECTED_WAITING]);

			return SMF_EVENT_HANDLED;
		}
	}

#if defined(CONFIG_APP_BUTTON)
	if (state_object->chan == &button_chan) {
		const struct button_msg *msg = (const struct button_msg *)state_object->msg_buf;

		if (msg->type == BUTTON_PRESS_SHORT) {
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_CONNECTED_SAMPLING]);

			return SMF_EVENT_HANDLED;
		}
	}
#endif /* CONFIG_APP_BUTTON */

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

	if (state_object->chan == &storage_chan) {
		const struct storage_msg *msg = (const struct storage_msg *)state_object->msg_buf;

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

	err = zbus_chan_pub(&led_chan, &led_msg, PUB_TIMEOUT);
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
	const enum app_state resume_state = state_object->running_history;

	if (state_object->chan == &fota_chan) {
		const struct fota_msg *msg = (const struct fota_msg *)state_object->msg_buf;

		switch (msg->type) {
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
	if (state_object->chan == &cloud_chan) {
		const struct cloud_msg *msg = (const struct cloud_msg *)state_object->msg_buf;

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

			return SMF_EVENT_HANDLED;
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

			return SMF_EVENT_HANDLED;
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

	if (state_object->chan == &fota_chan) {
		const struct fota_msg *msg = (const struct fota_msg *)state_object->msg_buf;

		switch (msg->type) {
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

	err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("Failed to publish network disconnect request, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static enum smf_state_result fota_waiting_for_network_disconnect_run(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	if (state_object->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		if (msg->type == NETWORK_DISCONNECTED) {
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

	err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("Failed to publish network disconnect request, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static enum smf_state_result fota_waiting_for_network_disconnect_to_apply_image_run(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	if (state_object->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		if (msg->type == NETWORK_DISCONNECTED) {
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
	struct fota_msg msg = { .type = FOTA_IMAGE_APPLY };

	err = zbus_chan_pub(&fota_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("Failed to publish FOTA image apply request, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static enum smf_state_result fota_applying_image_run(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	if (state_object->chan == &fota_chan) {
		const struct fota_msg *msg = (const struct fota_msg *)state_object->msg_buf;

		if (msg->type == FOTA_SUCCESS_REBOOT_NEEDED) {
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
	err = zbus_chan_pub(&storage_chan, &msg, PUB_TIMEOUT);
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

#if defined(CONFIG_APP_INSPECT_SHELL)
	main_state_ctx = &main_state;
#endif /* CONFIG_APP_INSPECT_SHELL */

	main_state.sample_interval_sec = CONFIG_APP_SAMPLING_INTERVAL_SECONDS;
	main_state.storage_threshold = CONFIG_APP_STORAGE_INITIAL_THRESHOLD;
	main_state.first_sample_pending = true;
	main_state.reconnect_backoff_sec = APP_RECONNECT_BACKOFF_INITIAL_SECONDS;

	k_work_init_delayable(&main_state.reconnect_work, reconnect_work_fn);
	k_work_init_delayable(&main_state.tn_recovery_work, tn_recovery_work_fn);

	LOG_DBG("Main has started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, task_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		SEND_FATAL_ERROR();

		return -EFAULT;
	}

	smf_set_initial(SMF_CTX(&main_state), &states[STATE_WAITING_FOR_MODULES_INIT]);

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
