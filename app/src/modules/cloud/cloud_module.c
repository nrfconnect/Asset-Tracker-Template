/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/smf.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <net/nrf_cloud.h>
#include <net/nrf_cloud_coap.h>
#include <app_version.h>

#if defined(CONFIG_MEMFAULT)
#include <memfault/core/trace_event.h>
#endif /* CONFIG_MEMFAULT */

#include "modules_common.h"
#include "message_channel.h"
#include "battery.h"
#include "network.h"

/* Register log module */
LOG_MODULE_REGISTER(cloud, CONFIG_APP_CLOUD_LOG_LEVEL);

#define CUSTOM_JSON_APPID_VAL_CONEVAL "CONEVAL"
#define CUSTOM_JSON_APPID_VAL_BATTERY "BATTERY"
#define MAX_MSG_SIZE	(MAX(sizeof(struct cloud_payload),					\
			 MAX(sizeof(struct network_msg),					\
			 MAX(sizeof(struct battery_msg), sizeof(struct environmental_msg)))))

BUILD_ASSERT(CONFIG_APP_CLOUD_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_CLOUD_EXEC_TIME_SECONDS_MAX,
	     "Watchdog timeout must be greater than maximum execution time");

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(cloud);

/* Observe channels */
ZBUS_CHAN_ADD_OBS(PAYLOAD_CHAN, cloud, 0);
ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, cloud, 0);
ZBUS_CHAN_ADD_OBS(BATTERY_CHAN, cloud, 0);
ZBUS_CHAN_ADD_OBS(TRIGGER_CHAN, cloud, 0);
ZBUS_CHAN_ADD_OBS(ENVIRONMENTAL_CHAN, cloud, 0);

/* Define channels provided by this module */

ZBUS_CHAN_DEFINE(PAYLOAD_CHAN,
		 struct cloud_payload,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

ZBUS_CHAN_DEFINE(CLOUD_CHAN,
		 enum cloud_msg_type,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 CLOUD_DISCONNECTED
);

/* Enumerator to be used in privat cloud channel */
enum priv_cloud_msg {
	CLOUD_BACKOFF_EXPIRED,
};

/* Create private cloud channel for internal messaging that is not intended for external use.
 * The channel is needed to communicate from asynchronous callbacks to the state machine and
 * ensure state transitions only happen from the cloud  module thread where the state machine
 * is running.
 */
ZBUS_CHAN_DEFINE(PRIV_CLOUD_CHAN,
		 enum priv_cloud_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(cloud),
		 CLOUD_BACKOFF_EXPIRED
);

/* Connection attempt backoff timer is run as a delayable work on the system workqueue */
static void backoff_timer_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(backoff_timer_work, backoff_timer_work_fn);

/* State machine definitions */
static const struct smf_state states[];

/* Forward declarations of state handlers */
static void state_running_entry(void *o);
static void state_running_run(void *o);

static void state_disconnected_entry(void *o);
static void state_disconnected_run(void *o);

static void state_connecting_entry(void *o);

static void state_connecting_attempt_entry(void *o);

static void state_connecting_backoff_entry(void *o);
static void state_connecting_backoff_run(void *o);
static void state_connecting_backoff_exit(void *o);

static void state_connected_entry(void *o);
static void state_connected_exit(void *o);

static void state_connected_ready_entry(void *o);
static void state_connected_ready_run(void *o);

static void state_connected_paused_entry(void *o);
static void state_connected_paused_run(void *o);

/* Defining the hierarchical cloud  module states:
 *
 *   STATE_RUNNING: The cloud  module has started and is running
 *      - STATE_DISCONNECTED: Cloud connection is not established
 *	- STATE_CONNECTING: The module is connecting to cloud
 *		- STATE_CONNECTING_ATTEMPT: The module is trying to connect to cloud
 *		- STATE_CONNECTING_BACKOFF: The module is waiting before trying to connect again
 *	- STATE_CONNECTED: Cloud connection has been established. Note that because of
 *			    connection ID being used, the connection is valid even though
 *			    network connection is intermittently lost (and socket is closed)
 *		- STATE_CONNECTED_READY: Connected to cloud and network connection, ready to send
 *		- STATE_CONNECTED_PAUSED: Connected to cloud, but not network connection
 */
enum cloud_module_state {
	STATE_RUNNING,
	STATE_DISCONNECTED,
	STATE_CONNECTING,
	STATE_CONNECTING_ATTEMPT,
	STATE_CONNECTING_BACKOFF,
	STATE_CONNECTED,
	STATE_CONNECTED_READY,
	STATE_CONNECTED_PAUSED,
};

/* Construct state table */
static const struct smf_state states[] = {
	[STATE_RUNNING] =
		SMF_CREATE_STATE(state_running_entry, state_running_run, NULL,
				 NULL, /* No parent state */
				 &states[STATE_DISCONNECTED]), /* Initial transition */

	[STATE_DISCONNECTED] =
		SMF_CREATE_STATE(state_disconnected_entry, state_disconnected_run, NULL,
				 &states[STATE_RUNNING],
				 NULL),

	[STATE_CONNECTING] = SMF_CREATE_STATE(
				state_connecting_entry, NULL, NULL,
				&states[STATE_RUNNING],
				&states[STATE_CONNECTING_ATTEMPT]),

	[STATE_CONNECTING_ATTEMPT] = SMF_CREATE_STATE(
				state_connecting_attempt_entry, NULL, NULL,
				&states[STATE_CONNECTING],
				NULL),

	[STATE_CONNECTING_BACKOFF] = SMF_CREATE_STATE(
				state_connecting_backoff_entry, state_connecting_backoff_run,
				state_connecting_backoff_exit,
				&states[STATE_CONNECTING],
				NULL),

	[STATE_CONNECTED] =
		SMF_CREATE_STATE(state_connected_entry, NULL, state_connected_exit,
				 &states[STATE_RUNNING],
				 &states[STATE_CONNECTED_READY]),

	[STATE_CONNECTED_READY] =
		SMF_CREATE_STATE(state_connected_ready_entry, state_connected_ready_run, NULL,
				 &states[STATE_CONNECTED],
				 NULL),

	[STATE_CONNECTED_PAUSED] =
		SMF_CREATE_STATE(state_connected_paused_entry, state_connected_paused_run,  NULL,
				 &states[STATE_CONNECTED],
				 NULL),
};

/* User defined state object.
 * Used to transfer data between state changes.
 */
static struct state_object {
	/* This must be first */
	struct smf_ctx ctx;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;

	/* Last received message */
	uint8_t msg_buf[MAX_MSG_SIZE];

	/* Network status */
	enum network_msg_type nw_status;

	/* Connection attempt counter. Reset when entering STATE_CONNECTING */
	uint32_t connection_attempts;

	/* Connection backoff time */
	uint32_t backoff_time;
} cloud_state;

/* Static helper function */
static void task_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void connect_to_cloud(void)
{
	int err;
	char buf[NRF_CLOUD_CLIENT_ID_MAX_LEN];

	err = nrf_cloud_client_id_get(buf, sizeof(buf));
	if (err == 0) {
		LOG_INF("Connecting to nRF Cloud CoAP with client ID: %s", buf);
	} else {
		LOG_ERR("nrf_cloud_client_id_get, error: %d, cannot continue", err);

		SEND_FATAL_ERROR();
		return;
	}

	err = nrf_cloud_coap_connect(APP_VERSION_STRING);
	if (err == 0) {
		STATE_SET(cloud_state, STATE_CONNECTED);

		return;
	}

	/* Connection failed, retry */
	LOG_ERR("nrf_cloud_coap_connect, error: %d", err);

	STATE_SET(cloud_state, STATE_CONNECTING_BACKOFF);
}

static uint32_t calculate_backoff_time(uint32_t attempts)
{
	uint32_t backoff_time = CONFIG_APP_CLOUD_BACKOFF_INITIAL_SECONDS;

	/* Calculate backoff time */
	if (IS_ENABLED(CONFIG_APP_CLOUD_BACKOFF_TYPE_EXPONENTIAL)) {
		backoff_time = CONFIG_APP_CLOUD_BACKOFF_INITIAL_SECONDS << (attempts - 1);
	} else if (IS_ENABLED(CONFIG_APP_CLOUD_BACKOFF_TYPE_LINEAR)) {
		backoff_time = CONFIG_APP_CLOUD_BACKOFF_INITIAL_SECONDS +
			((attempts - 1) * CONFIG_APP_CLOUD_BACKOFF_LINEAR_INCREMENT_SECONDS);
	}

	/* Limit backoff time */
	if (backoff_time > CONFIG_APP_CLOUD_BACKOFF_MAX_SECONDS) {
		backoff_time = CONFIG_APP_CLOUD_BACKOFF_MAX_SECONDS;
	}

	LOG_DBG("Backoff time: %u seconds", backoff_time);

	return backoff_time;
}

static void backoff_timer_work_fn(struct k_work *work)
{
	int err;
	enum priv_cloud_msg msg = CLOUD_BACKOFF_EXPIRED;

	err = zbus_chan_pub(&PRIV_CLOUD_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

/* Zephyr State Machine Framework handlers */

/* Handler for STATE_RUNNING */

static void state_running_entry(void *o)
{
	int err;

	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	err = nrf_cloud_coap_init();
	if (err) {
		LOG_ERR("nrf_cloud_coap_init, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static void state_running_run(void *o)
{
	struct state_object *state_object = o;

	LOG_DBG("%s", __func__);

	if (state_object->chan == &NETWORK_CHAN) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		if (msg.type == NETWORK_DISCONNECTED) {
			STATE_SET(cloud_state, STATE_DISCONNECTED);

			return;
		}
	}
}

/* Handlers for STATE_CLOUD_DISCONNECTED. */
static void state_disconnected_entry(void *o)
{
	int err;
	enum cloud_msg_type cloud_status = CLOUD_DISCONNECTED;

	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	err = zbus_chan_pub(&CLOUD_CHAN, &cloud_status, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static void state_disconnected_run(void *o)
{
	struct state_object const *state_object = o;

	LOG_DBG("%s", __func__);

	struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

	if ((state_object->chan == &NETWORK_CHAN) && (msg.type == NETWORK_CONNECTED)) {
		STATE_SET(cloud_state, STATE_CONNECTING);

		return;
	}
}

/* Handlers for STATE_CONNECTING */

static void state_connecting_entry(void *o)
{
	/* Reset connection attempts counter */
	struct state_object *state_object = o;

	LOG_DBG("%s", __func__);

	state_object->connection_attempts = 0;
}

/* Handler for STATE_CONNECTING_ATTEMPT */

static void state_connecting_attempt_entry(void *o)
{
	struct state_object *state_object = o;

	LOG_DBG("%s", __func__);

	state_object->connection_attempts++;

	connect_to_cloud();
}

/* Handler for STATE_CONNECTING_BACKOFF */

static void state_connecting_backoff_entry(void *o)
{
	struct state_object *state_object = o;

	LOG_DBG("%s", __func__);

	state_object->backoff_time = calculate_backoff_time(state_object->connection_attempts);

	k_work_schedule(&backoff_timer_work, K_SECONDS(state_object->backoff_time));
}

static void state_connecting_backoff_run(void *o)
{
	struct state_object *state_object = o;

	LOG_DBG("%s", __func__);

	if (state_object->chan == &PRIV_CLOUD_CHAN) {
		enum priv_cloud_msg msg = *(enum priv_cloud_msg *)state_object->msg_buf;

		if (msg == CLOUD_BACKOFF_EXPIRED) {
			STATE_SET(cloud_state, STATE_CONNECTING_ATTEMPT);

			return;
		}
	}
}

static void state_connecting_backoff_exit(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	k_work_cancel_delayable(&backoff_timer_work);
}

/* Handler for STATE_CLOUD_CONNECTED. */
static void state_connected_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);
	LOG_INF("Connected to Cloud");
}

static void state_connected_exit(void *o)
{
	int err;

	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	err = nrf_cloud_coap_disconnect();
	if (err && (err != -ENOTCONN)) {
		LOG_ERR("nrf_cloud_coap_disconnect, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

/* Handlers for STATE_CONNECTED_READY */

static void shadow_get(bool delta_only)
{
	int err;
	uint8_t recv_buf[CONFIG_APP_MODULE_RECV_BUFFER_SIZE] = { 0 };
	size_t recv_buf_len = sizeof(recv_buf);

	LOG_DBG("Requesting device shadow from the device");

	err = nrf_cloud_coap_shadow_get(recv_buf, &recv_buf_len, delta_only,
					COAP_CONTENT_FORMAT_APP_JSON);
	if (err == -EACCES) {
		LOG_WRN("Not connected, error: %d", err);
		return;
	} else if (err == -ETIMEDOUT) {
		LOG_WRN("Request timed out, error: %d", err);
		return;
	} else if (err > 0) {
		LOG_WRN("Cloud error: %d", err);

		IF_ENABLED(CONFIG_MEMFAULT,
			(MEMFAULT_TRACE_EVENT_WITH_STATUS(nrf_cloud_coap_shadow_get, err)));

		return;
	} else if (err) {
		LOG_ERR("Failed to request shadow delta: %d", err);
		return;
	}

	/* No further processing of shadow is implemented */
}

static void state_connected_ready_entry(void *o)
{
	int err;
	enum cloud_msg_type cloud_status = CLOUD_CONNECTED_READY_TO_SEND;

	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	err = zbus_chan_pub(&CLOUD_CHAN, &cloud_status, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	shadow_get(false);
}

static void state_connected_ready_run(void *o)
{
	int err;
	struct state_object *state_object = o;

	LOG_DBG("%s", __func__);

	if (state_object->chan == &NETWORK_CHAN) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_DISCONNECTED:
			STATE_SET(cloud_state, STATE_CONNECTED_PAUSED);
			break;

		case NETWORK_CONNECTED:
			STATE_EVENT_HANDLED(cloud_state);
			break;

		case NETWORK_QUALITY_SAMPLE_RESPONSE:
			err = nrf_cloud_coap_sensor_send(CUSTOM_JSON_APPID_VAL_CONEVAL,
							 msg.conn_eval_params.energy_estimate,
							 NRF_CLOUD_NO_TIMESTAMP, true);
			if (err) {
				LOG_ERR("nrf_cloud_coap_sensor_send, error: %d", err);
				SEND_FATAL_ERROR();
				return;
			}

			err = nrf_cloud_coap_sensor_send(NRF_CLOUD_JSON_APPID_VAL_RSRP,
							 msg.conn_eval_params.rsrp,
							 NRF_CLOUD_NO_TIMESTAMP, true);
			if (err) {
				LOG_ERR("nrf_cloud_coap_sensor_send, error: %d", err);
				SEND_FATAL_ERROR();
				return;
			}

			break;

		default:
			break;
		}
	}

	if (state_object->chan == &BATTERY_CHAN) {
		struct battery_msg msg = MSG_TO_BATTERY_MSG(state_object->msg_buf);

		if (msg.type == BATTERY_PERCENTAGE_SAMPLE_RESPONSE) {
			err = nrf_cloud_coap_sensor_send(CUSTOM_JSON_APPID_VAL_BATTERY,
							 msg.percentage,
							 NRF_CLOUD_NO_TIMESTAMP, true);
			if (err) {
				LOG_ERR("nrf_cloud_coap_sensor_send, error: %d", err);
				SEND_FATAL_ERROR();
			}

			return;
		}
	}

	if (state_object->chan == &ENVIRONMENTAL_CHAN) {
		struct environmental_msg msg = MSG_TO_ENVIRONMENTAL_MSG(state_object->msg_buf);

		if (msg.type == ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE) {
			err = nrf_cloud_coap_sensor_send(NRF_CLOUD_JSON_APPID_VAL_TEMP,
							 msg.temperature,
							 NRF_CLOUD_NO_TIMESTAMP, true);
			if (err) {
				LOG_ERR("nrf_cloud_coap_sensor_send, error: %d", err);
				SEND_FATAL_ERROR();
			}

			err = nrf_cloud_coap_sensor_send(NRF_CLOUD_JSON_APPID_VAL_AIR_PRESS,
							 msg.pressure,
							 NRF_CLOUD_NO_TIMESTAMP, true);
			if (err) {
				LOG_ERR("nrf_cloud_coap_sensor_send, error: %d", err);
				SEND_FATAL_ERROR();
			}

			err = nrf_cloud_coap_sensor_send(NRF_CLOUD_JSON_APPID_VAL_HUMID,
							 msg.humidity,
							 NRF_CLOUD_NO_TIMESTAMP, true);
			if (err) {
				LOG_ERR("nrf_cloud_coap_sensor_send, error: %d", err);
				SEND_FATAL_ERROR();
			}

			return;
		}
	}

	if (state_object->chan == &PAYLOAD_CHAN) {
		struct cloud_payload *payload = MSG_TO_PAYLOAD(state_object->msg_buf);

		err = nrf_cloud_coap_json_message_send(payload->buffer, false, false);
		if (err) {
			LOG_ERR("nrf_cloud_coap_json_message_send, error: %d", err);
			SEND_FATAL_ERROR();
		}
	}

	if (state_object->chan == &TRIGGER_CHAN) {
		const enum trigger_type type = MSG_TO_TRIGGER_TYPE(state_object->msg_buf);

		if (type == TRIGGER_POLL_SHADOW) {
			LOG_DBG("Poll trigger received");

			shadow_get(true);
		}
	}
}

/* Handlers for STATE_CONNECTED_PAUSED */

static void state_connected_paused_entry(void *o)
{
	int err;
	enum cloud_msg_type cloud_status = CLOUD_CONNECTED_PAUSED;

	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	err = zbus_chan_pub(&CLOUD_CHAN, &cloud_status, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static void state_connected_paused_run(void *o)
{
	struct state_object *state_object = o;
	struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

	LOG_DBG("%s", __func__);

	if ((state_object->chan == &NETWORK_CHAN) && (msg.type == NETWORK_CONNECTED)) {
		STATE_SET(cloud_state, STATE_CONNECTED_READY);

		return;
	}
}

/* End of state handlers */

static void cloud_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = (CONFIG_APP_CLOUD_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms = (CONFIG_APP_CLOUD_EXEC_TIME_SECONDS_MAX * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);

	LOG_DBG("cloud  module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, task_wdt_callback, (void *)k_current_get());

	/* Initialize the state machine to STATE_RUNNING, which will also run its entry function */
	STATE_SET_INITIAL(cloud_state, STATE_RUNNING);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}

		err = zbus_sub_wait_msg(&cloud, &cloud_state.chan, cloud_state.msg_buf,
					zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}

		err = STATE_RUN(cloud_state);
		if (err) {
			LOG_ERR("STATE_RUN(), error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}
	}
}

K_THREAD_DEFINE(cloud_module_thread_id,
		CONFIG_APP_CLOUD_THREAD_STACK_SIZE,
		cloud_module_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
