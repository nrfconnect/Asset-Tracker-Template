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

#include "modules_common.h"
#include "message_channel.h"

/* Register log module */
LOG_MODULE_REGISTER(transport, CONFIG_APP_TRANSPORT_LOG_LEVEL);


BUILD_ASSERT(CONFIG_APP_TRANSPORT_WATCHDOG_TIMEOUT_SECONDS >
			 CONFIG_APP_TRANSPORT_EXEC_TIME_SECONDS_MAX,
			 "Watchdog timeout must be greater than maximum execution time");

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(transport);

/* Observe channels */
ZBUS_CHAN_ADD_OBS(PAYLOAD_CHAN, transport, 0);
ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, transport, 0);

#define MAX_MSG_SIZE (MAX(sizeof(struct payload), sizeof(enum network_msg_type)))

/* Enumerator to be used in privat transport channel */
enum priv_transport_evt {
	IRRECOVERABLE_ERROR,
	CLOUD_CONN_SUCCES,
	CLOUD_CONN_RETRY,	/* Unused for now */
};

/* Create private transport channel for internal messaging */
ZBUS_CHAN_DEFINE(PRIV_TRANSPORT_CHAN,
		 enum priv_transport_evt,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(transport),
		 IRRECOVERABLE_ERROR
);

/* Forward declarations */
static const struct smf_state states[];

static void connect_work_fn(struct k_work *work);

static void state_running_entry(void *o);
static void state_running_run(void *o);

static void state_disconnected_entry(void *o);
static void state_disconnected_run(void *o);

static void state_connecting_entry(void *o);
static void state_connecting_run(void *o);

static void state_connected_entry(void *o);
static void state_connected_exit(void *o);

static void state_connected_ready_entry(void *o);
static void state_connected_ready_run(void *o);

static void state_connected_paused_entry(void *o);
static void state_connected_paused_run(void *o);

/* Defining the hierarchical transport module states:
 *
 *   STATE_RUNNING: The transport module has started and is running
 *       - STATE_DISCONNECTED: Cloud connection is not established
 *	 - STATE_CONNECTING: The module is connecting to cloud
 *	 - STATE_CONNECTED: Cloud connection has been established. Note that because of
 *			    connection ID being used, the connection is valid even though
 *			    network connection is intermittently lost (and socket is closed)
 *		- STATE_CONNECTED_READY: Connected to cloud and network connection, ready to send
 *		- STATE_CONNECTED_PAUSED: Connected to cloud, but not network connection
 */
enum cloud_module_state {
	STATE_RUNNING,
	STATE_DISCONNECTED,
	STATE_CONNECTING,
	STATE_CONNECTED,
	STATE_CONNECTED_READY,
	STATE_CONNECTED_PAUSED,
};

/* Construct state table */
static const struct smf_state states[] = {
	[STATE_RUNNING] =
		SMF_CREATE_STATE(state_running_entry, state_running_run, NULL,
				 NULL,
				 &states[STATE_DISCONNECTED]),

	[STATE_DISCONNECTED] =
		SMF_CREATE_STATE(state_disconnected_entry, state_disconnected_run, NULL,
				 &states[STATE_RUNNING],
				 NULL),

	[STATE_CONNECTING] = SMF_CREATE_STATE(
				state_connecting_entry, state_connecting_run, NULL,
				&states[STATE_RUNNING],
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
} transport_state;

/* Define connection work - Used to handle reconnection attempts to the cloud */
static K_WORK_DELAYABLE_DEFINE(connect_work, connect_work_fn);

/* Define stack_area of application workqueue */
static K_THREAD_STACK_DEFINE(stack_area, CONFIG_APP_TRANSPORT_WORKQUEUE_STACK_SIZE);

/* Declare application workqueue. This workqueue is used to connect to the cloud, and
 * schedule reconnectionn attempts upon connection loss.
 */
static struct k_work_q transport_queue;

static void task_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

/* Connect work - Used to establish a connection to the clpoud and schedule reconnection attempts */
static void connect_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	int err;
	char buf[NRF_CLOUD_CLIENT_ID_MAX_LEN];
	enum priv_transport_evt conn_result = CLOUD_CONN_SUCCES;

	err = nrf_cloud_client_id_get(buf, sizeof(buf));
	if (!err) {
		LOG_INF("Connecting to nRF Cloud CoAP with client ID: %s", buf);
	} else {
		LOG_ERR("nrf_cloud_client_id_get, error: %d, cannot continue", err);

		SEND_FATAL_ERROR();
		return;
	}

	err = nrf_cloud_coap_connect(APP_VERSION_STRING);
	if (err) {
		LOG_ERR("nrf_cloud_coap_connect, error: %d, retrying", err);
		goto retry;
	}

	err = zbus_chan_pub(&PRIV_TRANSPORT_CHAN, &conn_result, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

retry:
	k_work_reschedule_for_queue(&transport_queue, &connect_work,
			  K_SECONDS(CONFIG_APP_TRANSPORT_RECONNECTION_TIMEOUT_SECONDS));
}

static void connect_work_cancel(void)
{
	k_work_cancel_delayable(&connect_work);
}

/* Zephyr State Machine Framework handlers */

/* Handler for STATE_RUNNING */

static void state_running_entry(void *o)
{
	int err;

	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	/* Initialize and start application workqueue.
	 * This workqueue can be used to offload tasks and/or as a timer when wanting to
	 * schedule functionality using the 'k_work' API.
	 */
	k_work_queue_init(&transport_queue);
	k_work_queue_start(&transport_queue, stack_area,
			   K_THREAD_STACK_SIZEOF(stack_area),
			   K_LOWEST_APPLICATION_THREAD_PRIO,
			   NULL);

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
		enum network_msg_type nw_status = MSG_TO_NETWORK_STATUS(state_object->msg_buf);

		if (nw_status == NETWORK_DISCONNECTED) {
			STATE_SET(transport_state, STATE_DISCONNECTED);

			return;
		}
	}
}

/* Handlers for STATE_CLOUD_DISCONNECTED. */
static void state_disconnected_entry(void *o)
{
	int err;
	enum cloud_status cloud_status = CLOUD_DISCONNECTED;

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

	if ((state_object->chan == &NETWORK_CHAN) &&
	    (MSG_TO_NETWORK_STATUS(state_object->msg_buf) == NETWORK_CONNECTED)) {
		STATE_SET(transport_state, STATE_CONNECTING);

		return;
	}

	if (state_object->chan == &PAYLOAD_CHAN) {
		LOG_WRN("Discarding payload since we are not connected to cloud");
	}
}

/* Handlers for STATE_CONNECTING */

static void state_connecting_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	k_work_reschedule_for_queue(&transport_queue, &connect_work, K_NO_WAIT);
}

static void state_connecting_run(void *o)
{
	struct state_object *state_object = o;

	LOG_DBG("%s", __func__);

	if (state_object->chan == &PRIV_TRANSPORT_CHAN) {
		enum priv_transport_evt conn_result = *(enum priv_transport_evt *)state_object->msg_buf;

		if (conn_result == CLOUD_CONN_SUCCES) {
			STATE_SET(transport_state, STATE_CONNECTED);

			return;
		}
	}
}

/* Handler for STATE_CLOUD_CONNECTED. */
static void state_connected_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);
	LOG_INF("Connected to Cloud");

	/* Cancel any ongoing connect work when we enter STATE_CLOUD_CONNECTED */
	connect_work_cancel();
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

	connect_work_cancel();
}

/* Handlers for STATE_CONNECTED_READY */

static void state_connected_ready_entry(void *o)
{
	int err;
	enum cloud_status cloud_status = CLOUD_CONNECTED_READY_TO_SEND;

	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	err = zbus_chan_pub(&CLOUD_CHAN, &cloud_status, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static void state_connected_ready_run(void *o)
{
	struct state_object *state_object = o;

	LOG_DBG("%s", __func__);

	if (state_object->chan == &PRIV_TRANSPORT_CHAN) {
		enum priv_transport_evt conn_result =
			*(enum priv_transport_evt *)state_object->msg_buf;

		if (conn_result == CLOUD_CONN_RETRY) {
			STATE_SET(transport_state, STATE_CONNECTING);

			return;
		}
	}

	if (state_object->chan == &NETWORK_CHAN) {
		if (MSG_TO_NETWORK_STATUS(state_object->msg_buf) == NETWORK_DISCONNECTED) {
			STATE_SET(transport_state, STATE_CONNECTED_PAUSED);

			return;
		}

		if (MSG_TO_NETWORK_STATUS(state_object->msg_buf) == NETWORK_CONNECTED) {
			STATE_EVENT_HANDLED(transport_state);

			return;
		}
	}

	if (state_object->chan == &PAYLOAD_CHAN) {
		int err;
		struct payload *payload = MSG_TO_PAYLOAD(state_object->msg_buf);

		LOG_HEXDUMP_DBG(payload->buffer, MIN(payload->buffer_len, 32), "Payload");

		err = nrf_cloud_coap_bytes_send(payload->buffer, payload->buffer_len, false);
		if (err == -EACCES) {

			/* Not connected, retry connection */

			enum priv_transport_evt conn_result = CLOUD_CONN_RETRY;

			err = zbus_chan_pub(&PRIV_TRANSPORT_CHAN, &conn_result, K_SECONDS(1));
			if (err) {
				LOG_ERR("zbus_chan_pub, error: %d", err);
				SEND_FATAL_ERROR();
				return;
			}

		} else if (err) {
			LOG_ERR("nrf_cloud_coap_bytes_send, error: %d", err);
		}
	}
}

/* Handlers for STATE_CONNECTED_PAUSED */

static void state_connected_paused_entry(void *o)
{
	int err;
	enum cloud_status cloud_status = CLOUD_CONNECTED_PAUSED;

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

	LOG_DBG("%s", __func__);

	if ((state_object->chan == &NETWORK_CHAN) &&
	    (MSG_TO_NETWORK_STATUS(state_object->msg_buf) == NETWORK_CONNECTED)) {
		STATE_SET(transport_state, STATE_CONNECTED_READY);

		return;
	}
}

/* End of state handlers */


static void transport_task(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = (CONFIG_APP_TRANSPORT_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms = (CONFIG_APP_TRANSPORT_EXEC_TIME_SECONDS_MAX * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);

	LOG_DBG("Transport module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, task_wdt_callback, (void *)k_current_get());

	/* Initialize the state machine to STATE_RUNNING, which will also run its entry function */
	STATE_SET_INITIAL(transport_state, STATE_RUNNING);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}

		err = zbus_sub_wait_msg(&transport, &transport_state.chan, transport_state.msg_buf,
					zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}

		err = STATE_RUN(transport_state);
		if (err) {
			LOG_ERR("STATE_RUN(), error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}
	}
}

K_THREAD_DEFINE(transport_task_id,
		CONFIG_APP_TRANSPORT_THREAD_STACK_SIZE,
		transport_task, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
