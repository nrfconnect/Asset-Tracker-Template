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
#include <net/nrf_cloud_rest.h>
#include <net/nrf_provisioning.h>
#include <nrf_cloud_coap_transport.h>
#include <zephyr/net/coap.h>
#include <app_version.h>
#include <date_time.h>

#if defined(CONFIG_MEMFAULT)
#include <memfault/ports/zephyr/http.h>
#include <memfault/metrics/metrics.h>
#include <memfault/panics/coredump.h>
#endif /* CONFIG_MEMFAULT */

#include "cloud.h"
#include "app_common.h"
#include "network.h"
#include "storage.h"

/* Register log module */
LOG_MODULE_REGISTER(cloud, CONFIG_APP_CLOUD_LOG_LEVEL);

#define CUSTOM_JSON_APPID_VAL_CONEVAL "CONEVAL"
#define CUSTOM_JSON_APPID_VAL_BATTERY "BATTERY"

#define AGNSS_MAX_DATA_SIZE 3800

/* Prevent nRF Provisioning Shell from being used to trigger provisioning.
 * The cloud state machine does not support out of order provisioning via nRF Provisioning shell.
 */
BUILD_ASSERT(!IS_ENABLED(CONFIG_NRF_PROVISIONING_SHELL),
	     "nRF Provisioning Shell not supported, use att_cloud_provision shell command instead");

BUILD_ASSERT(CONFIG_APP_CLOUD_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_CLOUD_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

/* Register zbus subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(cloud_subscriber);

/* Define the channels that the module subscribes to, their associated message types
 * and the subscriber that will receive the messages on the channel.
 * ENVIRONMENTAL_CHAN, POWER_CHAN, and LOCATION_CHAN are optional and are only included if the
 * corresponding module is enabled.
 */
#define CHANNEL_LIST(X)										\
					 X(NETWORK_CHAN,	struct network_msg)		\
					 X(CLOUD_CHAN,		struct cloud_msg)		\
					 X(STORAGE_CHAN,	struct storage_msg)		\
					 X(STORAGE_DATA_CHAN,	struct storage_msg)

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
	CLOUD_CONNECTION_FAILED,
	CLOUD_CONNECTION_SUCCESS,
	CLOUD_NOT_AUTHENTICATED,
	CLOUD_PROVISIONING_FINISHED,
	CLOUD_PROVISIONING_FAILED,
	CLOUD_BACKOFF_EXPIRED,
	CLOUD_SEND_REQUEST_FAILED,
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

/* State machine */

/* Cloud module states */
enum cloud_module_state {
	/* The cloud module has started and is running */
	STATE_RUNNING,
		/* Cloud connection is not established */
		STATE_DISCONNECTED,
		/* The module is connecting to cloud */
		STATE_CONNECTING,
			/* The module is trying to connect to cloud */
			STATE_CONNECTING_ATTEMPT,
				/* Module is provisioned to nRF Cloud CoAP */
				STATE_PROVISIONED,
				/* The module is trying to provision to nRF Cloud CoAP using
				 * nRF Cloud Provisioning Service
				 */
				STATE_PROVISIONING,
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

/* Types of sections of the shadow document to poll for. */
enum shadow_poll_type {
	SHADOW_POLL_DELTA,
	SHADOW_POLL_DESIRED,
};

/* State object.
 * Used to transfer context data between state changes.
 */
struct cloud_state_object {
	/* This must be first */
	struct smf_ctx ctx;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;

	/* Last received message */
	uint8_t msg_buf[MAX_MSG_SIZE];

	/* Network status */
	enum network_msg_type nw_status;

	/* Provisioning ongoing flag */
	bool provisioning_ongoing;

	/* Connection attempt counter. Reset when entering STATE_CONNECTING */
	uint32_t connection_attempts;

	/* Connection backoff time */
	uint32_t backoff_time;
};

/* Forward declarations of state handlers */
static void state_running_entry(void *obj);
static void state_disconnected_entry(void *obj);
static void state_disconnected_run(void *obj);
static void state_connecting_entry(void *obj);
static void state_connecting_run(void *obj);
static void state_connecting_attempt_entry(void *obj);
static void state_connecting_provisioned_entry(void *obj);
static void state_connecting_provisioned_run(void *obj);
static void state_connecting_provisioning_entry(void *obj);
static void state_connecting_provisioning_run(void *obj);
static void state_connecting_backoff_entry(void *obj);
static void state_connecting_backoff_run(void *obj);
static void state_connecting_backoff_exit(void *obj);
static void state_connected_entry(void *obj);
static void state_connected_exit(void *obj);
static void state_connected_ready_entry(void *obj);
static void state_connected_ready_run(void *obj);
static void state_connected_paused_entry(void *obj);
static void state_connected_paused_run(void *obj);

/* Forward declarations of location handler */
#if defined(CONFIG_APP_LOCATION)
static void handle_location_message(const struct location_msg *msg);
#endif

/* State machine definition */
static const struct smf_state states[] = {
	[STATE_RUNNING] =
		SMF_CREATE_STATE(state_running_entry, NULL, NULL,
				 NULL, /* No parent state */
				 &states[STATE_DISCONNECTED]), /* Initial transition */

	[STATE_DISCONNECTED] =
		SMF_CREATE_STATE(state_disconnected_entry, state_disconnected_run, NULL,
				 &states[STATE_RUNNING],
				 NULL),

	[STATE_CONNECTING] =
		SMF_CREATE_STATE(state_connecting_entry, state_connecting_run, NULL,
				 &states[STATE_RUNNING],
				 &states[STATE_CONNECTING_ATTEMPT]),

	[STATE_CONNECTING_ATTEMPT] =
		SMF_CREATE_STATE(state_connecting_attempt_entry, NULL, NULL,
				 &states[STATE_CONNECTING],
				 &states[STATE_PROVISIONED]),

	[STATE_PROVISIONED] =
		SMF_CREATE_STATE(state_connecting_provisioned_entry,
				 state_connecting_provisioned_run,
				 NULL,
				 &states[STATE_CONNECTING_ATTEMPT],
				 NULL),

	[STATE_PROVISIONING] =
		SMF_CREATE_STATE(state_connecting_provisioning_entry,
				 state_connecting_provisioning_run, NULL,
				 &states[STATE_CONNECTING_ATTEMPT],
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

static void cloud_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void connect_to_cloud(const struct cloud_state_object *state_object)
{
	ARG_UNUSED(state_object);

	int err;
	char buf[NRF_CLOUD_CLIENT_ID_MAX_LEN];
	enum priv_cloud_msg msg = CLOUD_CONNECTION_FAILED;

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
		LOG_INF("nRF Cloud CoAP connection successful");

		msg = CLOUD_CONNECTION_SUCCESS;
	} else if (err == -EACCES || err == -ENOEXEC || err == -ECONNREFUSED) {
		LOG_WRN("nrf_cloud_coap_connect, error: %d", err);
		LOG_WRN("nRF Cloud CoAP connection failed, unauthorized or invalid credentials");

		msg = CLOUD_NOT_AUTHENTICATED;
	} else {
		LOG_WRN("nRF Cloud CoAP connection refused");

		msg = CLOUD_CONNECTION_FAILED;
	}

	err = zbus_chan_pub(&PRIV_CLOUD_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
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

static void send_request_failed(void)
{
	int err;
	enum priv_cloud_msg cloud_msg = CLOUD_SEND_REQUEST_FAILED;

	err = zbus_chan_pub(&PRIV_CLOUD_CHAN, &cloud_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void nrf_provisioning_callback(const struct nrf_provisioning_callback_data *event)
{
	int err;
	enum priv_cloud_msg msg = CLOUD_PROVISIONING_FINISHED;
	enum network_msg_type nw_msg = NETWORK_DISCONNECT;

	switch (event->type) {
	case NRF_PROVISIONING_EVENT_NEED_LTE_DEACTIVATED:
		LOG_WRN("nRF Provisioning requires device to deactivate LTE");

		nw_msg = NETWORK_DISCONNECT;

		err = zbus_chan_pub(&NETWORK_CHAN, &nw_msg, K_SECONDS(1));
		if (err) {
			LOG_ERR("zbus_chan_pub, error: %d", err);
			SEND_FATAL_ERROR();
		}

		return;
	case NRF_PROVISIONING_EVENT_NEED_LTE_ACTIVATED:
		LOG_WRN("nRF Provisioning requires device activate LTE");

		nw_msg = NETWORK_CONNECT;

		err = zbus_chan_pub(&NETWORK_CHAN, &nw_msg, K_SECONDS(1));
		if (err) {
			LOG_ERR("zbus_chan_pub, error: %d", err);
			SEND_FATAL_ERROR();
		}

		return;
	case NRF_PROVISIONING_EVENT_DONE:
		LOG_DBG("Provisioning finished");

		msg = CLOUD_PROVISIONING_FINISHED;
		k_sleep(K_SECONDS(10));
		break;
	case NRF_PROVISIONING_EVENT_NO_COMMANDS:
		LOG_WRN("No commands from the nRF Provisioning Service to process");
		LOG_WRN("Treating as provisioning finished");

		msg = CLOUD_PROVISIONING_FINISHED;

		/* Workaround: Wait some seconds before sending finished message.
		 * This is needed to be able to connect to getting authorized when connecting
		 * to nRF Cloud CoAP after provisioning. To be investigated further.
		 */
		k_sleep(K_SECONDS(10));
		break;
	case NRF_PROVISIONING_EVENT_FAILED_TOO_MANY_COMMANDS:
		LOG_ERR("Provisioning failed, too many commands for the device to handle");

		/* Provisioning failed due to receiving too many commands.
		 * Treat this as 'provisioning finished' to allow reconnection to nRF Cloud CoAP.
		 * The process will need to be restarted via the device shadow
		 * with an acceptable number of commands in the provisioning service list.
		 */
		msg = CLOUD_PROVISIONING_FINISHED;

		/* Workaround: Wait some seconds before sending finished message.
		 * This is needed to be able to connect to getting authorized when connecting
		 * to nRF Cloud CoAP after provisioning. To be investigated further.
		 */
		k_sleep(K_SECONDS(10));
		return;
	case NRF_PROVISIONING_EVENT_FAILED:
		LOG_ERR("Provisioning failed");

		msg = CLOUD_PROVISIONING_FAILED;
		break;
	case NRF_PROVISIONING_EVENT_FAILED_NO_VALID_DATETIME:
		LOG_ERR("Provisioning failed, no valid datetime reference");

		msg = CLOUD_PROVISIONING_FAILED;
		break;
	case NRF_PROVISIONING_EVENT_FAILED_DEVICE_NOT_CLAIMED:
		LOG_WRN("Provisioning failed, device not claimed");
		LOG_WRN("Claim the device using the device's attestation token on nrfcloud.com");
		LOG_WRN("\r\n\n%.*s.%.*s\r\n", event->token->attest_sz, event->token->attest,
					       event->token->cose_sz, event->token->cose);
		msg = CLOUD_PROVISIONING_FAILED;
		break;
	case NRF_PROVISIONING_EVENT_FAILED_WRONG_ROOT_CA:
		LOG_ERR("Provisioning failed, wrong CA certificate");

		SEND_FATAL_ERROR();
		return;
	case NRF_PROVISIONING_EVENT_FATAL_ERROR:
		LOG_ERR("Provisioning error");

		SEND_FATAL_ERROR();
		return;
	default:
		/* Don't care */
		return;
	}

	err = zbus_chan_pub(&PRIV_CLOUD_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

/* Storage handling functions */

static int send_storage_data_to_cloud(const struct storage_data_item *item)
{
	int err;
	int64_t timestamp_ms = NRF_CLOUD_NO_TIMESTAMP;
	const bool confirmable = IS_ENABLED(CONFIG_APP_CLOUD_CONFIRMABLE_MESSAGES);

	/* Get current timestamp */
	err = date_time_now(&timestamp_ms);
	if (err) {
		LOG_WRN("Failed to get current time, using no timestamp");
		timestamp_ms = NRF_CLOUD_NO_TIMESTAMP;
	}

#if defined(CONFIG_APP_POWER)
	if (item->type == STORAGE_TYPE_BATTERY) {
		double battery_percentage = item->data.BATTERY;

		err = nrf_cloud_coap_sensor_send(CUSTOM_JSON_APPID_VAL_BATTERY,
						 battery_percentage,
						 timestamp_ms,
						 confirmable);
		if (err) {
			LOG_ERR("Failed to send battery data to cloud, error: %d", err);
			return err;
		}

		LOG_DBG("Battery data sent to cloud: %.1f%%", battery_percentage);

		/* Unused variable if no other sources compiled in */
		(void)confirmable;

		return 0;
	}
#endif /* CONFIG_APP_POWER */

#if defined(CONFIG_APP_ENVIRONMENTAL)
	if (item->type == STORAGE_TYPE_ENVIRONMENTAL) {
		const struct environmental_msg *env = &item->data.ENVIRONMENTAL;

		err = nrf_cloud_coap_sensor_send(NRF_CLOUD_JSON_APPID_VAL_TEMP,
						 env->temperature,
						 timestamp_ms,
						 confirmable);
		if (err) {
			LOG_ERR("Failed to send temperature data to cloud, error: %d", err);
			return err;
		}

		err = nrf_cloud_coap_sensor_send(NRF_CLOUD_JSON_APPID_VAL_AIR_PRESS,
						 env->pressure,
						 timestamp_ms,
						 confirmable);
		if (err) {
			LOG_ERR("Failed to send pressure data to cloud, error: %d", err);
			return err;
		}

		err = nrf_cloud_coap_sensor_send(NRF_CLOUD_JSON_APPID_VAL_HUMID,
						 env->humidity,
						 timestamp_ms,
						 confirmable);
		if (err) {
			LOG_ERR("Failed to send humidity data to cloud, error: %d", err);
			return err;
		}

		LOG_DBG("Environmental data sent to cloud: T=%.1f°C, P=%.1fhPa, H=%.1f%%",
			(double)env->temperature, (double)env->pressure, (double)env->humidity);

		return 0;
	}
#endif /* CONFIG_APP_ENVIRONMENTAL */

#if defined(CONFIG_APP_LOCATION)
	if (item->type == STORAGE_TYPE_LOCATION) {
		const struct location_msg *loc = &item->data.LOCATION;

		handle_location_message(loc);

		return 0;
	}
#endif /* CONFIG_APP_LOCATION && CONFIG_LOCATION_METHOD_GNSS */

#if defined(CONFIG_APP_NETWORK)
	if (item->type == STORAGE_TYPE_NETWORK) {
		const struct network_msg *net = &item->data.NETWORK;

		handle_network_data_message(net);

		return 0;
	}
#endif /* CONFIG_APP_NETWORK */

	LOG_WRN("Unknown storage data type: %d", item->type);

	/* Unused variable if no data sources are enabled */
	(void)confirmable;

	return -ENOTSUP;
}

static int request_storage_batch_data(uint32_t session_id)
{
	int err;
	struct storage_msg msg = {
		.type = STORAGE_BATCH_REQUEST,
		.session_id = session_id,
	};

	LOG_DBG("Requesting storage batch data, session_id: 0x%X", msg.session_id);

	err = zbus_chan_pub(&STORAGE_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("Failed to request storage batch data, error: %d", err);

		return err;
	}

	return 0;
}

static void handle_storage_batch_available(const struct storage_msg *msg)
{
	int err;
	struct storage_data_item item;
	uint32_t items_processed = 0;
	uint32_t items_available = msg->data_len;
	uint32_t session_id = msg->session_id;
	struct storage_msg close_msg = {
		.type = STORAGE_BATCH_CLOSE,
		.session_id = session_id,
	};
	bool session_error = false;

	LOG_INF("Processing storage batch: %u items available", items_available);

	/* Drain the batch buffer: read until timeout, abort on hard error */
	while (!session_error) {
		err = storage_batch_read(&item, K_MSEC(500));
		if (err == -EAGAIN) {
			LOG_DBG("No more data available in batch (timeout)");

			break;
		} else if (err) {
			LOG_ERR("storage_batch_read failed, error: %d", err);

			session_error = true;

			continue;
		}

		/* Success: send the data item to cloud */
		err = send_storage_data_to_cloud(&item);
		if (err) {
			LOG_ERR("Failed to send storage data to cloud, error: %d", err);
		}

		items_processed++;
	}

	LOG_DBG("Processed %u/%u storage items", items_processed, items_available);

	if (!session_error && msg->more_data) {
		LOG_DBG("More data available in batch, requesting next batch");

		err = request_storage_batch_data(session_id);
		if (err) {
			LOG_ERR("Failed to request next storage batch data, error: %d", err);
		}

		return;
	}

	/* Close the batch session */
	err = zbus_chan_pub(&STORAGE_CHAN, &close_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("Failed to close storage batch session, error: %d", err);
	}
}

static void handle_storage_batch_empty(const struct storage_msg *msg)
{
	int err;
	struct storage_msg close_msg = {
		.type = STORAGE_BATCH_CLOSE,
		.session_id = msg->session_id,
	};

	LOG_DBG("Storage batch is empty, closing session");

	err = zbus_chan_pub(&STORAGE_CHAN, &close_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("Failed to close empty storage batch session, error: %d", err);
	}
}

static void handle_storage_batch_error(const struct storage_msg *msg)
{
	int err;
	struct storage_msg close_msg = {
		.type = STORAGE_BATCH_CLOSE,
		.session_id = msg->session_id,
	};

	LOG_ERR("Storage batch error occurred, closing session");

	err = zbus_chan_pub(&STORAGE_CHAN, &close_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("Failed to close error storage batch session, error: %d", err);
	}
}

static void handle_storage_batch_busy(const struct storage_msg *msg)
{
	ARG_UNUSED(msg);
	LOG_WRN("Storage batch is busy, will retry later");
	/* Could implement retry logic here if needed */
}

static void handle_storage_data(const struct storage_msg *msg)
{
	int err;
	/* Handle real-time storage data (from flush or passthrough mode) */
	struct storage_data_item item;

	/* Extract data from the storage message buffer */
	if (msg->data_len > sizeof(item.data)) {
		LOG_ERR("Storage data too large: %d bytes", msg->data_len);
		return;
	}

	item.type = msg->data_type;

	memcpy(&item.data, msg->buffer, msg->data_len);

	/* Send to cloud */
	err = send_storage_data_to_cloud(&item);
	if (err) {
		LOG_ERR("Failed to send real-time storage data to cloud, error: %d", err);
	}
}

/* State handlers */

static void state_running_entry(void *obj)
{
	int err;

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	err = nrf_cloud_coap_init();
	if (err) {
		LOG_ERR("nrf_cloud_coap_init, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	err = nrf_provisioning_init(nrf_provisioning_callback);
	if (err) {
		LOG_ERR("nrf_provisioning_init, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static void state_disconnected_entry(void *obj)
{
	int err;
	struct cloud_msg cloud_msg = {
		.type = CLOUD_DISCONNECTED,
	};

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static void state_disconnected_run(void *obj)
{
	struct cloud_state_object const *state_object = obj;
	struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

	if ((state_object->chan == &NETWORK_CHAN) && (msg.type == NETWORK_CONNECTED)) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTING]);

		return;
	}
}

static void state_connecting_entry(void *obj)
{
	/* Reset connection attempts counter */
	struct cloud_state_object *state_object = obj;

	LOG_DBG("%s", __func__);

	state_object->connection_attempts = 0;
	state_object->provisioning_ongoing = false;
}

static void state_connecting_run(void *obj)
{
	struct cloud_state_object *state_object = obj;

	if (state_object->chan == &NETWORK_CHAN) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		if (msg.type == NETWORK_DISCONNECTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED]);

			return;
		}
	}
}

static void state_connecting_attempt_entry(void *obj)
{
	struct cloud_state_object *state_object = obj;

	LOG_DBG("%s", __func__);

	state_object->connection_attempts++;
}

static void state_connecting_provisioned_entry(void *obj)
{
	struct cloud_state_object *state_object = obj;

	LOG_DBG("%s", __func__);

	state_object->provisioning_ongoing = false;

	connect_to_cloud(state_object);
}

static void state_connecting_provisioned_run(void *obj)
{
	struct cloud_state_object *state_object = obj;

	if (state_object->chan == &PRIV_CLOUD_CHAN) {
		enum priv_cloud_msg msg = *(const enum priv_cloud_msg *)state_object->msg_buf;

		if (msg == CLOUD_NOT_AUTHENTICATED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_PROVISIONING]);

			return;
		} else if (msg == CLOUD_CONNECTION_SUCCESS) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED]);

			return;
		} else if (msg == CLOUD_CONNECTION_FAILED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTING_BACKOFF]);

			return;
		}
	}
}

static void state_connecting_provisioning_entry(void *obj)
{
	int err;

	struct cloud_state_object *state_object = obj;

	LOG_DBG("%s", __func__);

	state_object->provisioning_ongoing = true;

	err = nrf_provisioning_trigger_manually();
	if (err) {
		LOG_ERR("nrf_provisioning_trigger_manually, error: %d", err);

		SEND_FATAL_ERROR();
		return;
	}
}

static void state_connecting_provisioning_run(void *obj)
{
	struct cloud_state_object *state_object = obj;

	if (state_object->chan == &PRIV_CLOUD_CHAN) {
		enum priv_cloud_msg msg = *(const enum priv_cloud_msg *)state_object->msg_buf;

		if (msg == CLOUD_PROVISIONING_FINISHED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_PROVISIONED]);

			return;
		} else if (msg == CLOUD_PROVISIONING_FAILED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTING_BACKOFF]);

			return;
		}
	}

	/* Its expected that the device goes online/offline a few times during provisioning.
	 * Therefore we handle network connected/disconnected events in this state preventing it
	 * from propagating up the state machine changing the cloud module's connectivity status.
	 */
	if (state_object->chan == &NETWORK_CHAN) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		if (msg.type == NETWORK_DISCONNECTED || msg.type == NETWORK_CONNECTED) {
			smf_set_handled(SMF_CTX(state_object));

			return;
		}
	}
}

static void state_connecting_backoff_entry(void *obj)
{
	int err;
	struct cloud_state_object *state_object = obj;

	LOG_DBG("%s", __func__);

	state_object->backoff_time = calculate_backoff_time(state_object->connection_attempts);

	LOG_WRN("Connection attempt failed, backoff time: %u seconds",
		state_object->backoff_time);

	err = k_work_schedule(&backoff_timer_work, K_SECONDS(state_object->backoff_time));
	if (err < 0) {
		LOG_ERR("k_work_schedule, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void state_connecting_backoff_run(void *obj)
{
	struct cloud_state_object const *state_object = obj;

	if (state_object->chan == &PRIV_CLOUD_CHAN) {
		const enum priv_cloud_msg msg = *(const enum priv_cloud_msg *)state_object->msg_buf;

		/* If the backoff timer expired, we can either continue provisioning or
		 * connect to cloud if already provisioned. The provisioning ongoing flag helps us
		 * determine what substate of connecting attempt we are attempting to enter.
		 */
		if ((msg == CLOUD_BACKOFF_EXPIRED) && !state_object->provisioning_ongoing) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_PROVISIONED]);

			return;
		} else if ((msg == CLOUD_BACKOFF_EXPIRED) && state_object->provisioning_ongoing) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_PROVISIONING]);

			return;
		}
	}
}

static void state_connecting_backoff_exit(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	(void)k_work_cancel_delayable(&backoff_timer_work);
}

static void state_connected_entry(void *obj)
{
	ARG_UNUSED(obj);

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

static void state_connected_exit(void *obj)
{
	int err;
	struct cloud_msg cloud_msg = {
		.type = CLOUD_DISCONNECTED,
	};

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	err = nrf_cloud_coap_disconnect();
	if (err && (err != -ENOTCONN && err != -EPERM)) {
		LOG_ERR("nrf_cloud_coap_disconnect, error: %d", err);
		SEND_FATAL_ERROR();
	}

	err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

#if defined(CONFIG_APP_LOCATION)
/* Handle cloud location requests from the location module */
static void handle_cloud_location_request(const struct location_data_cloud *request)
{
	int err;
	struct nrf_cloud_location_config loc_config = {
		.do_reply = false,
	};
	struct nrf_cloud_rest_location_request loc_req = {
		.config = &loc_config,
	};
	struct nrf_cloud_location_result result = { 0 };

	LOG_DBG("Handling cloud location request");

#if defined(CONFIG_LOCATION_METHOD_CELLULAR)
	if (request->cell_data != NULL) {
		/* NOSONAR: Cast away const qualifier is required due to API design mismatch
		 * between location library (const pointers) and nRF Cloud API (non-const pointers).
		 * The underlying nrf_cloud_coap_location_get function only reads the data.
		 */
		loc_req.cell_info = (struct lte_lc_cells_info *)request->cell_data; /* NOSONAR */

		LOG_DBG("Cellular data present: current cell ID: %d, neighbor cells: %d",
			request->cell_data->current_cell.id,
			request->cell_data->ncells_count);
	}
#endif

#if defined(CONFIG_LOCATION_METHOD_WIFI)
	if (request->wifi_data != NULL) {
		/* NOSONAR: Cast away const qualifier is required due to API design mismatch
		 * between location library (const pointers) and nRF Cloud API (non-const pointers).
		 * The underlying nrf_cloud_coap_location_get function only reads the data.
		 */
		loc_req.wifi_info = (struct wifi_scan_info *)request->wifi_data; /* NOSONAR */

		LOG_DBG("Wi-Fi data present: %d APs", request->wifi_data->cnt);
	}
#endif

	/* Send location request to nRF Cloud */
	err = nrf_cloud_coap_location_get(&loc_req, &result);
	if (err == COAP_RESPONSE_CODE_NOT_FOUND) {
		LOG_WRN("nRF Cloud CoAP location coordinates not found, error: %d", err);
		location_cloud_location_ext_result_set(LOCATION_EXT_RESULT_ERROR, NULL);

		return;
	} else if (err) {
		LOG_ERR("nrf_cloud_coap_location_get, error: %d", err);
		location_cloud_location_ext_result_set(LOCATION_EXT_RESULT_ERROR, NULL);

		send_request_failed();
		return;
	}
}

#if defined(CONFIG_NRF_CLOUD_AGNSS)
/* Handle A-GNSS data requests from the location module */
static void handle_agnss_request(const struct nrf_modem_gnss_agnss_data_frame *request)
{
	int err;
	static char agnss_buf[AGNSS_MAX_DATA_SIZE];
	struct nrf_cloud_rest_agnss_request agnss_req = {
		.type = NRF_CLOUD_REST_AGNSS_REQ_CUSTOM,
		.agnss_req = (struct nrf_modem_gnss_agnss_data_frame *)request,
		.net_info = NULL,
		.filtered = false,
		.mask_angle = 0
	};
	struct nrf_cloud_rest_agnss_result result = {
		.buf = agnss_buf,
		.buf_sz = sizeof(agnss_buf),
		.agnss_sz = 0
	};

	LOG_DBG("Handling A-GNSS data request");

	/* Send A-GNSS request to nRF Cloud */
	err = nrf_cloud_coap_agnss_data_get(&agnss_req, &result);
	if (err) {
		LOG_ERR("nrf_cloud_coap_agnss_data_get, error: %d", err);

		send_request_failed();
		return;
	}

	LOG_DBG("A-GNSS data received, size: %d bytes", result.agnss_sz);

	/* Process the A-GNSS data */
	err = location_agnss_data_process(result.buf, result.agnss_sz);
	if (err) {
		LOG_ERR("Failed to process A-GNSS data, error: %d", err);
		return;
	}

	LOG_DBG("A-GNSS data processed successfully");
}
#endif /* CONFIG_NRF_CLOUD_AGNSS */

#if defined(CONFIG_LOCATION_METHOD_GNSS)
/* Handle GNSS location data from the location module */
static void handle_gnss_location_data(const struct location_data *location_data)
{
	int err;
	int64_t timestamp_ms = NRF_CLOUD_NO_TIMESTAMP;
	bool confirmable = IS_ENABLED(CONFIG_APP_CLOUD_CONFIRMABLE_MESSAGES);
	struct nrf_cloud_gnss_data gnss_data = {
		.type = NRF_CLOUD_GNSS_TYPE_PVT,
		.ts_ms = timestamp_ms,
		.pvt = {
			.lat = location_data->latitude,
			.lon = location_data->longitude,
			.accuracy = location_data->accuracy,
		}
	};

	LOG_DBG("Handling GNSS location data: lat: %f, lon: %f, acc: %f",
		(double)location_data->latitude,
		(double)location_data->longitude,
		(double)location_data->accuracy);

	/* Get current timestamp */
	err = date_time_now(&timestamp_ms);
	if (err) {
		LOG_WRN("Failed to get current time");

		timestamp_ms = NRF_CLOUD_NO_TIMESTAMP;
	}

	gnss_data.ts_ms = timestamp_ms;

#if defined(CONFIG_LOCATION_DATA_DETAILS)
#define CLOUD_GNSS_HEADING_ACC_LIMIT (float)60.0

	struct location_data_details_gnss gnss = location_data->details.gnss;

	/* If detailed GNSS data is available, include altitude, speed, and heading */
	if (gnss.pvt_data.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
		gnss_data.pvt.alt = gnss.pvt_data.altitude;
		gnss_data.pvt.speed = gnss.pvt_data.speed;
		gnss_data.pvt.heading = gnss.pvt_data.heading;
		gnss_data.pvt.has_alt = 1;
		gnss_data.pvt.has_speed =
			(gnss.pvt_data.flags & NRF_MODEM_GNSS_PVT_FLAG_VELOCITY_VALID) ? 1 : 0;
		gnss_data.pvt.has_heading =
			(gnss.pvt_data.heading_accuracy < CLOUD_GNSS_HEADING_ACC_LIMIT) ? 1 : 0;
	}
#endif /* CONFIG_LOCATION_DATA_DETAILS */

	/* Send GNSS location data to nRF Cloud */
	err = nrf_cloud_coap_location_send(&gnss_data, confirmable);
	if (err) {
		LOG_ERR("nrf_cloud_coap_location_send, error: %d", err);
		send_request_failed();
		return;
	}

	LOG_INF("GNSS location data sent to nRF Cloud successfully");
}
#endif /* CONFIG_LOCATION_METHOD_GNSS */
#endif /* CONFIG_APP_LOCATION */

static void shadow_poll(enum shadow_poll_type type)
{
	int err;
	struct cloud_msg msg = {
		.type = (type == SHADOW_POLL_DELTA) ? CLOUD_SHADOW_RESPONSE_DELTA :
						      CLOUD_SHADOW_RESPONSE_DESIRED,
		.response = {
			.buffer_data_len = sizeof(msg.response.buffer),
		},
	};

	LOG_DBG("Requesting device shadow from the device");

	err = nrf_cloud_coap_shadow_get(msg.response.buffer,
					&msg.response.buffer_data_len,
					(type == SHADOW_POLL_DELTA) ? true : false,
					COAP_CONTENT_FORMAT_APP_CBOR);
	if (err) {
		LOG_ERR("nrf_cloud_coap_shadow_get, error: %d", err);

		send_request_failed();
		return;
	}

	if (msg.response.buffer_data_len == 0) {
		LOG_DBG("Shadow %s section not present",
			(type == SHADOW_POLL_DELTA) ? "delta" : "desired");
		return;
	}

	/* Workaroud: Sometimes nrf_cloud_coap_shadow_get() returns 0 even though obtaining
	 * the shadow failed. Ignore the payload if the first 10 bytes are zero.
	 */
	if (!memcmp(msg.response.buffer, "\0\0\0\0\0\0\0\0\0\0", 10)) {
		LOG_WRN("Returned buffer is empty, ignore");
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
		LOG_ERR("nrf_cloud_coap_patch, error: %d", err);

		send_request_failed();
		return;
	}

	err = zbus_chan_pub(&CLOUD_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

static void state_connected_ready_entry(void *obj)
{
	int err;
	static bool first = true;
	struct cloud_msg cloud_msg = {
		.type = CLOUD_CONNECTED,
	};

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	/* After an established connection, we poll the entire desired section to get our latest
	 * configuration. Do this only for the initial connection, not on every
	 * reconnection.
	 */
	if (!first) {
		return;
	}

	shadow_poll(SHADOW_POLL_DESIRED);

	first = false;
}

static void handle_priv_cloud_message(struct cloud_state_object const *state_object)
{
	enum priv_cloud_msg msg = *(const enum priv_cloud_msg *)state_object->msg_buf;

	if (msg == CLOUD_SEND_REQUEST_FAILED) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTING]);
	}
}

#if defined(CONFIG_APP_NETWORK)
static void handle_network_data_message(const struct network_msg *msg)
{
	int err;
	bool confirmable = IS_ENABLED(CONFIG_APP_CLOUD_CONFIRMABLE_MESSAGES);

	if (msg->type != NETWORK_QUALITY_SAMPLE_RESPONSE) {
		return;
	}

	err = nrf_cloud_coap_sensor_send(CUSTOM_JSON_APPID_VAL_CONEVAL,
					msg->conn_eval_params.energy_estimate,
					NRF_CLOUD_NO_TIMESTAMP,
					confirmable);
	if (err) {
		LOG_ERR("nrf_cloud_coap_sensor_send, error: %d", err);
		send_request_failed();

		return;
	}

	err = nrf_cloud_coap_sensor_send(NRF_CLOUD_JSON_APPID_VAL_RSRP,
					msg->conn_eval_params.rsrp,
					NRF_CLOUD_NO_TIMESTAMP,
					confirmable);
	if (err) {
		LOG_ERR("nrf_cloud_coap_sensor_send, error: %d", err);
		send_request_failed();
	}
}
#endif /* CONFIG_APP_NETWORK */

#if defined(CONFIG_APP_LOCATION)
static void handle_location_message(const struct location_msg *msg)
{
	switch (msg->type) {
	case LOCATION_CLOUD_REQUEST:
		LOG_DBG("Cloud location request received");
		handle_cloud_location_request(&msg->cloud_request);
		break;

#if defined(CONFIG_NRF_CLOUD_AGNSS)
	case LOCATION_AGNSS_REQUEST:
		LOG_DBG("A-GNSS data request received");
		handle_agnss_request(&msg->agnss_request);
		break;
#endif /* CONFIG_NRF_CLOUD_AGNSS */

#if defined(CONFIG_LOCATION_METHOD_GNSS)
	case LOCATION_GNSS_DATA:
		LOG_DBG("GNSS location data received");
		handle_gnss_location_data(&msg->gnss_data);
		break;
#endif /* CONFIG_LOCATION_METHOD_GNSS */

	default:
		break;
	}
}
#endif /* CONFIG_APP_LOCATION */

static void handle_storage_channel_message(struct cloud_state_object const *state_object)
{
	const struct storage_msg *msg = MSG_TO_STORAGE_MSG(state_object->msg_buf);

	switch (msg->type) {
	case STORAGE_BATCH_AVAILABLE:
		LOG_DBG("Storage batch available, %d items, session_id: 0x%X",
			msg->data_len, msg->session_id);
		handle_storage_batch_available(msg);
		break;
	case STORAGE_BATCH_EMPTY:
		LOG_DBG("Storage batch empty, session_id: 0x%X", msg->session_id);
		handle_storage_batch_empty(msg);
		break;
	case STORAGE_BATCH_ERROR:
		LOG_ERR("Storage batch error, session_id: 0x%X", msg->session_id);
		handle_storage_batch_error(msg);
		break;
	case STORAGE_BATCH_BUSY:
		LOG_WRN("Storage batch busy, session_id: 0x%X", msg->session_id);
		handle_storage_batch_busy(msg);
		break;
	default:
		break;
	}
}

static void handle_storage_data_message(struct cloud_state_object const *state_object)
{
	const struct storage_msg *msg = MSG_TO_STORAGE_MSG(state_object->msg_buf);

	if (msg->type == STORAGE_DATA) {
		LOG_DBG("Storage data received, type: %d, size: %d",
			msg->data_type, msg->data_len);
		handle_storage_data(msg);
	}
}

static void handle_cloud_channel_message(struct cloud_state_object const *state_object)
{
	int err;
	const struct cloud_msg *msg = MSG_TO_CLOUD_MSG_PTR(state_object->msg_buf);
	const bool confirmable = IS_ENABLED(CONFIG_APP_CLOUD_CONFIRMABLE_MESSAGES);

	switch (msg->type) {
	case CLOUD_PAYLOAD_JSON:
		err = nrf_cloud_coap_json_message_send(msg->payload.buffer,
						       false, confirmable);
		if (err) {
			LOG_ERR("nrf_cloud_coap_json_message_send, error: %d", err);
			send_request_failed();
		}
		break;
	case CLOUD_POLL_SHADOW:
		LOG_DBG("Poll shadow trigger received");
		/* On shadow poll requests, we only poll for delta changes. This is because
		 * we have gotten our entire desired section when polling the shadow
		 * on an established connection.
		 */
		shadow_poll(SHADOW_POLL_DELTA);
		break;
	case CLOUD_PROVISIONING_REQUEST:
		LOG_DBG("Provisioning request received");
		smf_set_state(SMF_CTX(state_object), &states[STATE_PROVISIONING]);
		break;
	default:
		break;
	}
}

static void state_connected_ready_run(void *obj)
{
	struct cloud_state_object const *state_object = obj;

	if (state_object->chan == &PRIV_CLOUD_CHAN) {
		handle_priv_cloud_message(state_object);
		return;
	}

	if (state_object->chan == &NETWORK_CHAN) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_DISCONNECTED:
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_PAUSED]);
			break;
		case NETWORK_CONNECTED:
			smf_set_handled(SMF_CTX(state_object));
			break;
		default:
			break;
		}

		return;
	}

	if (state_object->chan == &STORAGE_CHAN) {
		handle_storage_channel_message(state_object);
		return;
	}

	if (state_object->chan == &STORAGE_DATA_CHAN) {
		handle_storage_data_message(state_object);
		return;
	}

	if (state_object->chan == &CLOUD_CHAN) {
		handle_cloud_channel_message(state_object);
	}
}

/* Handlers for STATE_CONNECTED_PAUSED */

static void state_connected_paused_entry(void *obj)
{
	int err;
	struct cloud_msg cloud_msg = {
		.type = CLOUD_DISCONNECTED,
	};

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static void state_connected_paused_run(void *obj)
{
	struct cloud_state_object const *state_object = obj;
	struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

	if ((state_object->chan == &NETWORK_CHAN) && (msg.type == NETWORK_CONNECTED)) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_READY]);

		return;
	}
}

static void cloud_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = (CONFIG_APP_CLOUD_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_CLOUD_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	struct cloud_state_object cloud_state = { 0 };

	LOG_DBG("Cloud module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, cloud_wdt_callback, (void *)k_current_get());
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

K_THREAD_DEFINE(cloud_module_thread_id,
		CONFIG_APP_CLOUD_THREAD_STACK_SIZE,
		cloud_module_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
