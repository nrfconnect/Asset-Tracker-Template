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
#include <nrf_cloud_coap_transport.h>
#include <zephyr/net/coap.h>
#include <app_version.h>

#if defined(CONFIG_MEMFAULT)
#include <memfault/ports/zephyr/http.h>
#include <memfault/metrics/metrics.h>
#include <memfault/panics/coredump.h>
#endif /* CONFIG_MEMFAULT */

#include "cloud.h"
#include "app_common.h"
#include "network.h"

#if defined(CONFIG_APP_POWER)
#include "power.h"
#endif /* CONFIG_APP_POWER */

#if defined(CONFIG_APP_ENVIRONMENTAL)
#include "environmental.h"
#endif /* CONFIG_APP_ENVIRONMENTAL */

/* Register log module */
LOG_MODULE_REGISTER(cloud, CONFIG_APP_CLOUD_LOG_LEVEL);

#define CUSTOM_JSON_APPID_VAL_CONEVAL "CONEVAL"
#define CUSTOM_JSON_APPID_VAL_BATTERY "BATTERY"

BUILD_ASSERT(CONFIG_APP_CLOUD_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_CLOUD_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

/* Register zbus subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(cloud_subscriber);

/* Define the channels that the module subscribes to, their associated message types
 * and the subscriber that will receive the messages on the channel.
 * ENVIRONMENTAL_CHAN and POWER_CHAN are optional and are only included if the
 * corresponding module is enabled.
 */
#define CHANNEL_LIST(X)										\
					 X(NETWORK_CHAN,	struct network_msg)		\
					 X(CLOUD_CHAN,		struct cloud_msg)		\
IF_ENABLED(CONFIG_APP_ENVIRONMENTAL,	(X(ENVIRONMENTAL_CHAN,	struct environmental_msg)))	\
IF_ENABLED(CONFIG_APP_POWER,		(X(POWER_CHAN,		struct power_msg)))

/* Calculate the maximum message size from the list of channels */
#define MAX_MSG_SIZE			MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

/* Add the cloud_subscriber as observer to all the channels in the list. */
#define ADD_OBSERVERS(_chan, _type)	ZBUS_CHAN_ADD_OBS(_chan, cloud_subscriber, 0);

/*
 * Expand to a call to ZBUS_CHAN_ADD_OBS for each channel in the list.
 * Example: ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, cloud_subscriber, 0);
 */
CHANNEL_LIST(ADD_OBSERVERS)

ZBUS_CHAN_DEFINE(CLOUD_CHAN,
		 struct cloud_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.type = CLOUD_DISCONNECTED)
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
		 ZBUS_OBSERVERS(cloud_subscriber),
		 CLOUD_BACKOFF_EXPIRED
);

/* Connection attempt backoff timer is run as a delayable work on the system workqueue */
static void backoff_timer_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(backoff_timer_work, backoff_timer_work_fn);

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

/* Defining the hierarchical cloud  module states: */
enum cloud_module_state {
	/* The cloud module has started and is running */
	STATE_RUNNING,
		/* Cloud connection is not established */
		STATE_DISCONNECTED,
		/* The module is connecting to cloud */
		STATE_CONNECTING,
			/* The module is trying to connect to cloud */
			STATE_CONNECTING_ATTEMPT,
			/* The module is waiting before trying to connect again */
			STATE_CONNECTING_BACKOFF,
		/* Cloud connection has been established. Note that because of
		 * connection ID being used, the connection is valid even though
		 * network connection is intermittently lost (and socket is closed)
		 */
		STATE_CONNECTED,
			/* Connected to cloud and network connection, ready to send data */
			STATE_CONNECTED_READY,
			/* Connected to cloud, but not network connection */
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

	[STATE_CONNECTING] =
		SMF_CREATE_STATE(state_connecting_entry, NULL, NULL,
				 &states[STATE_RUNNING],
				 &states[STATE_CONNECTING_ATTEMPT]),

	[STATE_CONNECTING_ATTEMPT] =
		SMF_CREATE_STATE(state_connecting_attempt_entry, NULL, NULL,
				 &states[STATE_CONNECTING],
				 NULL),

	[STATE_CONNECTING_BACKOFF] =
		SMF_CREATE_STATE(state_connecting_backoff_entry, state_connecting_backoff_run,
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

/* Cloud module state object.
 * Used to transfer data between state changes.
 */
struct cloud_state {
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
};

/* Static helper function */
static void task_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void connect_to_cloud(const struct cloud_state *state_object)
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
		smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED]);

		return;
	}

	/* Connection failed, retry */
	LOG_ERR("nrf_cloud_coap_connect, error: %d", err);

	smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTING_BACKOFF]);
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

	ARG_UNUSED(work);

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
	const struct cloud_state *state_object = (const struct cloud_state *)o;

	if (state_object->chan == &NETWORK_CHAN) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		if (msg.type == NETWORK_DISCONNECTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED]);

			return;
		}
	}
}

/* Handlers for STATE_DISCONNECTED. */
static void state_disconnected_entry(void *o)
{
	int err;
	struct cloud_msg cloud_msg = {
		.type = CLOUD_DISCONNECTED,
	};

	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static void state_disconnected_run(void *o)
{
	const struct cloud_state *state_object = (const struct cloud_state *)o;
	struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

	if ((state_object->chan == &NETWORK_CHAN) && (msg.type == NETWORK_CONNECTED)) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTING]);

		return;
	}
}

/* Handlers for STATE_CONNECTING */

static void state_connecting_entry(void *o)
{
	/* Reset connection attempts counter */
	struct cloud_state *state_object = o;

	LOG_DBG("%s", __func__);

	state_object->connection_attempts = 0;
}

/* Handler for STATE_CONNECTING_ATTEMPT */

static void state_connecting_attempt_entry(void *o)
{
	struct cloud_state *state_object = o;

	LOG_DBG("%s", __func__);

	state_object->connection_attempts++;

	connect_to_cloud(state_object);
}

/* Handler for STATE_CONNECTING_BACKOFF */

static void state_connecting_backoff_entry(void *o)
{
	int err;
	struct cloud_state *state_object = o;

	LOG_DBG("%s", __func__);

	state_object->backoff_time = calculate_backoff_time(state_object->connection_attempts);

	err = k_work_schedule(&backoff_timer_work, K_SECONDS(state_object->backoff_time));
	if (err < 0) {
		LOG_ERR("k_work_schedule, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void state_connecting_backoff_run(void *o)
{
	const struct cloud_state *state_object = (const struct cloud_state *)o;

	if (state_object->chan == &PRIV_CLOUD_CHAN) {
		const enum priv_cloud_msg msg = *(const enum priv_cloud_msg *)state_object->msg_buf;

		if (msg == CLOUD_BACKOFF_EXPIRED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTING_ATTEMPT]);

			return;
		}
	}
}

static void state_connecting_backoff_exit(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	(void)k_work_cancel_delayable(&backoff_timer_work);
}

/* Handler for STATE_CONNECTED. */
static void state_connected_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);
	LOG_INF("Connected to Cloud");

#if defined(CONFIG_MEMFAULT)
	if (memfault_coredump_has_valid_coredump(NULL)) {
		/* Initial update to Memfault is handled internally in the
		 * Memfault LTE coredump layer.
		 */
		return;
	}

	/* No coredump available, trigger an initial update to Memfault. */
	(void)memfault_metrics_heartbeat_debug_trigger();
	(void)memfault_zephyr_port_post_data();
#endif /* CONFIG_MEMFAULT */
}

static void state_connected_exit(void *o)
{
	int err;

	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	err = nrf_cloud_coap_disconnect();
	if (err && (err != -ENOTCONN && err != -EPERM)) {
		LOG_ERR("nrf_cloud_coap_disconnect, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

/* Handlers for STATE_CONNECTED_READY */

static void shadow_get(bool delta_only)
{
	int err;
	struct cloud_msg msg = {
		.type = CLOUD_SHADOW_RESPONSE,
		.response = {
			.buffer_data_len = sizeof(msg.response.buffer),
		},
	};

	LOG_DBG("Requesting device shadow from the device");

	err = nrf_cloud_coap_shadow_get(msg.response.buffer,
					&msg.response.buffer_data_len,
					delta_only,
					COAP_CONTENT_FORMAT_APP_CBOR);
	if (err == -EACCES) {
		LOG_WRN("Not connected, error: %d", err);
		return;
	} else if (err == -ETIMEDOUT) {
		LOG_WRN("Request timed out, error: %d", err);
		return;
	} else if (err == -ENETUNREACH) {
		LOG_WRN("Network is unreachable, error: %d", err);
		return;
	} else if (err > 0) {
		LOG_WRN("Cloud error: %d", err);
		return;
	} else if (err == -E2BIG) {
		LOG_WRN("The provided buffer is not large enough, error: %d", err);
		return;
	} else if (err) {
		LOG_ERR("Failed to request shadow delta: %d", err);
		return;
	}

	if (msg.response.buffer_data_len == 0) {
		LOG_DBG("No shadow delta changes available");
		return;
	}

	/* Workaroud: Sometimes nrf_cloud_coap_shadow_get() returns 0 even though obtaining
	 * the shadow failed. Ignore the payload if the first 10 bytes are zero.
	 */
	if (!memcmp(msg.response.buffer, "\0\0\0\0\0\0\0\0\0\0", 10)) {
		LOG_WRN("Returned buffeør is empty, ignore");
		return;
	}

	err = zbus_chan_pub(&CLOUD_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	/* Clear the shadow delta by reporting the same data back to the shadow reported state  */
	err = nrf_cloud_coap_patch("state/reported", NULL,
				   msg.response.buffer,
				   msg.response.buffer_data_len,
				   COAP_CONTENT_FORMAT_APP_CBOR,
				   true,
				   NULL,
				   NULL);
	if (err) {
		LOG_ERR("Failed to patch the device shadow, error: %d", err);
		return;
	}
}

static void state_connected_ready_entry(void *o)
{
	int err;
	struct cloud_msg cloud_msg = {
		.type = CLOUD_CONNECTED,
	};

	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_SECONDS(1));
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
	const struct cloud_state *state_object = (const struct cloud_state *)o;
	bool confirmable = IS_ENABLED(CONFIG_APP_CLOUD_CONFIRMABLE_MESSAGES);

	if (state_object->chan == &NETWORK_CHAN) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_DISCONNECTED:
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_PAUSED]);
			break;

		case NETWORK_CONNECTED:
			smf_set_handled(SMF_CTX(state_object));
			break;

		case NETWORK_QUALITY_SAMPLE_RESPONSE:
			err = nrf_cloud_coap_sensor_send(CUSTOM_JSON_APPID_VAL_CONEVAL,
							 msg.conn_eval_params.energy_estimate,
							 NRF_CLOUD_NO_TIMESTAMP,
							 confirmable);
			if (err == -ENETUNREACH) {
				LOG_WRN("Network is unreachable, error: %d", err);
				return;
			} else if (err) {
				LOG_ERR("nrf_cloud_coap_sensor_send, error: %d", err);
				SEND_FATAL_ERROR();
				return;
			}

			err = nrf_cloud_coap_sensor_send(NRF_CLOUD_JSON_APPID_VAL_RSRP,
							 msg.conn_eval_params.rsrp,
							 NRF_CLOUD_NO_TIMESTAMP,
							 confirmable);
			if (err == -ENETUNREACH) {
				LOG_WRN("Network is unreachable, error: %d", err);
				return;
			} else if (err) {
				LOG_ERR("nrf_cloud_coap_sensor_send, error: %d", err);
				SEND_FATAL_ERROR();
				return;
			}

			break;

		default:
			break;
		}
	}

#if defined(CONFIG_APP_POWER)
	if (state_object->chan == &POWER_CHAN) {
		struct power_msg msg = MSG_TO_POWER_MSG(state_object->msg_buf);

		if (msg.type == POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE) {
			err = nrf_cloud_coap_sensor_send(CUSTOM_JSON_APPID_VAL_BATTERY,
							 msg.percentage,
							 NRF_CLOUD_NO_TIMESTAMP,
							 confirmable);
			if (err == -ENETUNREACH) {
				LOG_WRN("Network is unreachable, error: %d", err);
				return;
			} else if (err) {
				LOG_ERR("nrf_cloud_coap_sensor_send, error: %d", err);
				SEND_FATAL_ERROR();
				return;
			}

			return;
		}
	}
#endif /* CONFIG_APP_POWER */

#if defined(CONFIG_APP_ENVIRONMENTAL)
	if (state_object->chan == &ENVIRONMENTAL_CHAN) {
		struct environmental_msg msg = MSG_TO_ENVIRONMENTAL_MSG(state_object->msg_buf);

		if (msg.type == ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE) {
			err = nrf_cloud_coap_sensor_send(NRF_CLOUD_JSON_APPID_VAL_TEMP,
							 msg.temperature,
							 NRF_CLOUD_NO_TIMESTAMP,
							 confirmable);
			if (err == -ENETUNREACH) {
				LOG_WRN("Network is unreachable, error: %d", err);
				return;
			} else if (err) {
				LOG_ERR("nrf_cloud_coap_sensor_send, error: %d", err);
				SEND_FATAL_ERROR();
				return;
			}

			err = nrf_cloud_coap_sensor_send(NRF_CLOUD_JSON_APPID_VAL_AIR_PRESS,
							 msg.pressure,
							 NRF_CLOUD_NO_TIMESTAMP,
							 confirmable);
			if (err == -ENETUNREACH) {
				LOG_WRN("Network is unreachable, error: %d", err);
				return;
			} else if (err) {
				LOG_ERR("nrf_cloud_coap_sensor_send, error: %d", err);
				SEND_FATAL_ERROR();
				return;
			}

			err = nrf_cloud_coap_sensor_send(NRF_CLOUD_JSON_APPID_VAL_HUMID,
							 msg.humidity,
							 NRF_CLOUD_NO_TIMESTAMP,
							 confirmable);
			if (err == -ENETUNREACH) {
				LOG_WRN("Network is unreachable, error: %d", err);
				return;
			} else if (err) {
				LOG_ERR("nrf_cloud_coap_sensor_send, error: %d", err);
				SEND_FATAL_ERROR();
				return;
			}

			return;
		}
	}
#endif /* CONFIG_APP_ENVIRONMENTAL */

	if (state_object->chan == &CLOUD_CHAN) {
		const struct cloud_msg *msg = MSG_TO_CLOUD_MSG_PTR(state_object->msg_buf);

		if (msg->type == CLOUD_PAYLOAD_JSON) {
			err = nrf_cloud_coap_json_message_send(msg->payload.buffer,
							       false, confirmable);
			if (err == -ENETUNREACH) {
				LOG_WRN("Network is unreachable, error: %d", err);
				return;
			} else if (err) {
				LOG_ERR("nrf_cloud_coap_sensor_send, error: %d", err);
				SEND_FATAL_ERROR();
				return;
			}
		} else if (msg->type == CLOUD_POLL_SHADOW) {
			LOG_DBG("Poll shadow trigger received");

			shadow_get(true);
		}
	}
}

/* Handlers for STATE_CONNECTED_PAUSED */

static void state_connected_paused_entry(void *o)
{
	int err;
	struct cloud_msg cloud_msg = {
		.type = CLOUD_DISCONNECTED,
	};

	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static void state_connected_paused_run(void *o)
{
	const struct cloud_state *state_object = (const struct cloud_state *)o;
	struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

	if ((state_object->chan == &NETWORK_CHAN) && (msg.type == NETWORK_CONNECTED)) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_READY]);

		return;
	}
}

/* End of state handlers */

static void cloud_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = (CONFIG_APP_CLOUD_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_CLOUD_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	struct cloud_state cloud_state = { 0 };

	LOG_DBG("cloud  module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, task_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		SEND_FATAL_ERROR();
		return;
	}

	/* Initialize the state machine to STATE_RUNNING, which will also run its entry function */
	smf_set_initial(SMF_CTX(&cloud_state), &states[STATE_RUNNING]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}

		err = zbus_sub_wait_msg(&cloud_subscriber, &cloud_state.chan, cloud_state.msg_buf,
					zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}

		err = smf_run_state(SMF_CTX(&cloud_state));
		if (err) {
			LOG_ERR("smf_run_state(), error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}
	}
}

K_THREAD_DEFINE(cloud_thread_id,
		CONFIG_APP_CLOUD_THREAD_STACK_SIZE,
		cloud_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
