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
#include <date_time.h>

#include "app_common.h"
#include "cloud.h"
#include "cloud_internal.h"
#include "cloud_configuration.h"
#include "cloud_provisioning.h"
#include "cloud_location.h"
#ifdef CONFIG_APP_ENVIRONMENTAL
#include "cloud_environmental.h"
#endif /* CONFIG_APP_ENVIRONMENTAL */
#include "network.h"
#include "storage.h"

/* Register log module */
LOG_MODULE_REGISTER(cloud, CONFIG_APP_CLOUD_LOG_LEVEL);

#define CUSTOM_JSON_APPID_VAL_BATTERY "BATTERY"
#define AGNSS_MAX_DATA_SIZE 3800

/* Prevent nRF Provisioning Shell from being used to trigger provisioning.
 * The cloud state machine does not support out of order provisioning via nRF Provisioning shell.
 */
BUILD_ASSERT(!IS_ENABLED(CONFIG_NRF_PROVISIONING_SHELL),
	"nRF Provisioning Shell not supported, use 'att_cloud provision' shell command instead");

BUILD_ASSERT(CONFIG_APP_CLOUD_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_CLOUD_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

/* Register zbus subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(cloud_subscriber);

/* Define the channels that the module subscribes to, their associated message types
 * and the subscriber that will receive the messages on the channel.
 */
#define CHANNEL_LIST(X)						\
	X(network_chan,		struct network_msg)		\
	X(cloud_chan,		struct cloud_msg)		\
	X(storage_chan,		struct storage_msg)		\
	X(location_chan,	struct location_msg)		\
	X(storage_data_chan,	struct storage_msg)

/* Calculate the maximum message size from the list of channels */
#define MAX_MSG_SIZE			MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

/* Add the cloud_subscriber as observer to all the channels in the list. */
#define ADD_OBSERVERS(_chan, _type)	ZBUS_CHAN_ADD_OBS(_chan, cloud_subscriber, 0);

/*
 * Expand to a call to ZBUS_CHAN_ADD_OBS for each channel in the list.
 * Example: ZBUS_CHAN_ADD_OBS(network_chan, cloud_subscriber, 0);
 */
CHANNEL_LIST(ADD_OBSERVERS)

ZBUS_CHAN_DEFINE(cloud_chan,
		 struct cloud_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Create private cloud channel for internal messaging that is not intended for external use.
 * The channel is needed to communicate from asynchronous callbacks to the state machine and
 * ensure state transitions only happen from the cloud  module thread where the state machine
 * is running.
 */
ZBUS_CHAN_DEFINE(priv_cloud_chan,
		 struct priv_cloud_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(cloud_subscriber),
		 ZBUS_MSG_INIT(0)
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
				/* Connected and idle, waiting for data to send */
				STATE_CONNECTED_IDLE,
				/* Connected and actively draining storage batch */
				STATE_CONNECTED_SENDING_DATA,
			/* Connected to cloud, but not network connection */
			STATE_CONNECTED_PAUSED,
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

	/* Last network connection status */
	bool network_connected;

	/* Provisioning ongoing flag */
	bool provisioning_ongoing;

	/* Connection attempt counter. Reset when entering STATE_CONNECTING */
	uint32_t connection_attempts;

	/* Connection backoff time */
	uint32_t backoff_time;

	/* Active storage batch context */
	uint32_t storage_batch_session_id;
};

/* Result of processing one storage batch item in STATE_CONNECTED_SENDING_DATA. */
enum batch_drain_result {
	/* More items may follow; remain in STATE_CONNECTED_SENDING_DATA. */
	BATCH_DRAIN_CONTINUE,
	/* End-of-batch marker reached; all items delivered successfully. */
	BATCH_DRAIN_DONE,
	/* Read or network error; stop draining without completing the batch. */
	BATCH_DRAIN_ABORTED,
};

/* Forward declarations of state handlers */
static void state_running_entry(void *obj);
static enum smf_state_result state_running_run(void *obj);
static void state_disconnected_entry(void *obj);
static enum smf_state_result state_disconnected_run(void *obj);
static void state_connecting_entry(void *obj);
static enum smf_state_result state_connecting_run(void *obj);
static void state_connecting_attempt_entry(void *obj);
static void state_connecting_provisioned_entry(void *obj);
static enum smf_state_result state_connecting_provisioned_run(void *obj);
static void state_connecting_provisioning_entry(void *obj);
static enum smf_state_result state_connecting_provisioning_run(void *obj);
static void state_connecting_backoff_entry(void *obj);
static enum smf_state_result state_connecting_backoff_run(void *obj);
static void state_connecting_backoff_exit(void *obj);
static void state_connected_entry(void *obj);
static void state_connected_exit(void *obj);
static void state_connected_ready_entry(void *obj);
static enum smf_state_result state_connected_ready_run(void *obj);
static enum smf_state_result state_connected_idle_run(void *obj);
static void state_connected_sending_data_entry(void *obj);
static enum smf_state_result state_connected_sending_data_run(void *obj);
static void state_connected_sending_data_exit(void *obj);
static void state_connected_paused_entry(void *obj);
static enum smf_state_result state_connected_paused_run(void *obj);

/* State machine definition */
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
				 &states[STATE_CONNECTED_IDLE]),

	[STATE_CONNECTED_IDLE] =
		SMF_CREATE_STATE(NULL, state_connected_idle_run, NULL,
				 &states[STATE_CONNECTED_READY],
				 NULL),

	[STATE_CONNECTED_SENDING_DATA] =
		SMF_CREATE_STATE(state_connected_sending_data_entry,
				 state_connected_sending_data_run,
				 state_connected_sending_data_exit,
				 &states[STATE_CONNECTED_READY],
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
	struct priv_cloud_msg msg = { .type = CLOUD_CONNECTION_FAILED };

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

		msg.type = CLOUD_CONNECTION_SUCCESS;
	} else if (err == -EACCES || err == -ENOEXEC || err == -ECONNREFUSED) {
		LOG_WRN("nrf_cloud_coap_connect, error: %d", err);
		LOG_WRN("nRF Cloud CoAP connection failed, unauthorized or invalid credentials");

		msg.type = CLOUD_NOT_AUTHENTICATED;
	} else {
		LOG_WRN("nRF Cloud CoAP connection refused");

		msg.type = CLOUD_CONNECTION_FAILED;
	}

	err = zbus_chan_pub(&priv_cloud_chan, &msg, PUB_TIMEOUT);
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

	__ASSERT(backoff_time <= CONFIG_APP_CLOUD_BACKOFF_MAX_SECONDS,
		 "Backoff time exceeds maximum configured backoff time");

	LOG_DBG("Backoff time: %u seconds", backoff_time);

	return backoff_time;
}

static void backoff_timer_work_fn(struct k_work *work)
{
	int err;
	const struct priv_cloud_msg msg = { .type = CLOUD_BACKOFF_EXPIRED };

	ARG_UNUSED(work);

	err = zbus_chan_pub(&priv_cloud_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void send_request_failed(void)
{
	int err;
	const struct priv_cloud_msg cloud_msg = { .type = CLOUD_SEND_REQUEST_FAILED };

	err = zbus_chan_pub(&priv_cloud_chan, &cloud_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void storage_batch_item_process_trigger(void)
{
	int err;
	const struct priv_cloud_msg msg = { .type = CLOUD_STORAGE_BATCH_ITEM_PROCESS };

	err = zbus_chan_pub(&priv_cloud_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void storage_batch_close(struct cloud_state_object *state_object)
{
	if (state_object->storage_batch_session_id == 0U) {
		return;
	}

	int err;
	struct storage_msg out_msg = {
		.type = STORAGE_BATCH_CLOSE,
		.session_id = state_object->storage_batch_session_id,
	};

	err = zbus_chan_pub(&storage_chan, &out_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("Failed to close storage batch session, error: %d", err);
		SEND_FATAL_ERROR();
	}

	state_object->storage_batch_session_id = 0U;
}

static void storage_batch_consume_item(uint32_t session_id, enum storage_data_type data_type)
{
	int err;
	struct storage_msg out_msg = {
		.type = STORAGE_BATCH_CONSUME,
		.session_id = session_id,
		.data_type = data_type,
	};

	err = zbus_chan_pub(&storage_chan, &out_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("Failed to consume storage item, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void send_provisioned_msg(void)
{
	int err;
	const struct cloud_msg cloud_msg = {
		.type = CLOUD_PROVISIONED,
	};

	err = zbus_chan_pub(&cloud_chan, &cloud_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

/**
 * @brief Attempt to set the provided uptime (in milliseconds) to unix time.
 *
 * Tries to convert the provided timestamp from uptime to unix time in milliseconds, if needed.
 * If it can't convert it will stay unchanged.
 *
 * @param uptime_ms Uptime to convert to unix time.
 * @return int 0 if conversion was successful,
 *             -EINVAL if the provided pointer is NULL,
 *             -EALREADY if the provided time was already in unix time (>= 2026-01-01),
 *             -ENODATA if date time is not valid,
 */
static inline int attempt_timestamp_to_unix_ms(int64_t *uptime_ms)
{
	int err;

	if (uptime_ms == NULL) {
		return -EINVAL;
	}

	if (*uptime_ms >= UNIX_TIME_MS_2026_01_01) {
		/* Already unix time */
		return -EALREADY;
	}

	if (*uptime_ms > k_uptime_get()) {
		/* Uptime cannot be in the future */
		return -EINVAL;
	}

	if (!date_time_is_valid()) {
		/* Cannot convert without valid time */
		return -ENODATA;
	}

	err = date_time_uptime_to_unix_time_ms(uptime_ms);
	if (err) {
		return err;
	}

	return 0;
}

#if (defined(CONFIG_APP_POWER) || defined(CONFIG_APP_ENVIRONMENTAL))
static int handle_data_timestamp(int64_t *timestamp_ms)
{
	int err;

	/* Soft attempt to convert uptime to unix time, keep original value on failure */
	err = attempt_timestamp_to_unix_ms(timestamp_ms);
	if (err == 0 || err == -EALREADY) {
		return 0;
	}

	if (IS_ENABLED(CONFIG_APP_CLOUD_HANDLE_WRONG_SAMPLE_TIMESTAMPS_KEEP)) {
		LOG_WRN("Keeping original timestamp value");
		return 0;
	} else if (IS_ENABLED(CONFIG_APP_CLOUD_HANDLE_WRONG_SAMPLE_TIMESTAMPS_NOW)) {
		*timestamp_ms = k_uptime_get();

		err = attempt_timestamp_to_unix_ms(timestamp_ms);
		if (err) {
			LOG_ERR("Failed to set timestamp to current time, error: %d", err);
			return err;
		}

		LOG_WRN("Setting timestamp to current time");
		return 0;
	} else if (IS_ENABLED(CONFIG_APP_CLOUD_HANDLE_WRONG_SAMPLE_TIMESTAMPS_NO_TIMESTAMP)) {
		*timestamp_ms = NRF_CLOUD_NO_TIMESTAMP;
		return 0;
	} else if (IS_ENABLED(CONFIG_APP_CLOUD_HANDLE_WRONG_SAMPLE_TIMESTAMPS_DROP)) {
		LOG_WRN("Dropping data with invalid timestamp");
		return 0;
	} else {
		/* Default behavior: APP_CLOUD_HANDLE_WRONG_SAMPLE_TIMESTAMPS_DROP */
		return err;
	}
}
#endif /* CONFIG_APP_POWER || CONFIG_APP_ENVIRONMENTAL */

/* Storage handling functions */

static int send_storage_data_to_cloud(const struct storage_data_item *item)
{
	int err;
	int64_t timestamp_ms = NRF_CLOUD_NO_TIMESTAMP;
	const bool confirmable = IS_ENABLED(CONFIG_APP_CLOUD_CONFIRMABLE_MESSAGES);

#if defined(CONFIG_APP_POWER)
	if (item->type == STORAGE_TYPE_BATTERY) {
		const struct power_msg *power = &item->data.BATTERY;

		/* Convert timestamp to unix time */
		timestamp_ms = power->timestamp;

		err = handle_data_timestamp(&timestamp_ms);
		if (err) {
			return err;
		}

		err = nrf_cloud_coap_sensor_send(CUSTOM_JSON_APPID_VAL_BATTERY,
						 power->percentage,
						 timestamp_ms,
						 confirmable);
		if (err) {
			LOG_ERR("Failed to send battery data to cloud, error: %d", err);
			return err;
		}

		LOG_DBG("Battery data sent to cloud: %.1f%%", power->percentage);

		/* Unused variable if no other sources compiled in */
		(void)confirmable;

		return 0;
	}
#endif /* CONFIG_APP_POWER */

#if defined(CONFIG_APP_ENVIRONMENTAL)
	if (item->type == STORAGE_TYPE_ENVIRONMENTAL) {
		const struct environmental_msg *env = &item->data.ENVIRONMENTAL;

		/* Convert timestamp to unix time */
		timestamp_ms = env->timestamp;

		err = handle_data_timestamp(&timestamp_ms);
		if (err) {
			return err;
		}

		return cloud_environmental_send(env, timestamp_ms, confirmable);
	}
#endif /* CONFIG_APP_ENVIRONMENTAL */

#if defined(CONFIG_APP_LOCATION)
	if (item->type == STORAGE_TYPE_LOCATION) {
		const struct location_msg *loc = &item->data.LOCATION;

		return cloud_location_handle_message(loc);
	}
#endif /* CONFIG_APP_LOCATION && CONFIG_LOCATION_METHOD_GNSS */

	LOG_WRN("Unknown storage data type: %d", item->type);

	/* Unused variables if no data sources are enabled */
	(void)confirmable;
	(void)timestamp_ms;
	(void) err;

	return -ENOTSUP;
}

static void handle_storage_batch_empty(const struct storage_msg *msg)
{
	int err;
	struct storage_msg close_msg = {
		.type = STORAGE_BATCH_CLOSE,
		.session_id = msg->session_id,
	};

	LOG_DBG("Storage batch is empty, closing session");

	err = zbus_chan_pub(&storage_chan, &close_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("Failed to close empty storage batch session, error: %d", err);
		SEND_FATAL_ERROR();
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

	err = zbus_chan_pub(&storage_chan, &close_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("Failed to close error storage batch session, error: %d", err);
		SEND_FATAL_ERROR();
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
	/* Handle real-time storage data */
	struct storage_data_item item;

	/* Extract data from the storage message buffer */
	if (msg->data_len > sizeof(item.data)) {
		LOG_ERR("Storage data too large: %d bytes", msg->data_len);
		SEND_FATAL_ERROR();
		return;
	}

	item.type = msg->data_type;

	memcpy(&item.data, msg->buffer, msg->data_len);

	/* Send to cloud */
	err = send_storage_data_to_cloud(&item);
	if (err) {
		LOG_ERR("Failed to send real-time storage data to cloud, error: %d", err);
		return;
	}
}

static void handle_cloud_channel_message(struct cloud_state_object const *state_object)
{
	int err;
	const struct cloud_msg *msg = (const struct cloud_msg *)state_object->msg_buf;
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
	case CLOUD_SHADOW_GET_DELTA:
		LOG_DBG("Poll shadow delta trigger received");
		err = cloud_configuration_poll(SHADOW_POLL_DELTA);
		if (err) {
			LOG_ERR("cloud_configuration_poll, error: %d", err);
			send_request_failed();
		}
		break;
	case CLOUD_SHADOW_GET_DESIRED:
		LOG_DBG("Poll shadow desired trigger received");
		err = cloud_configuration_poll(SHADOW_POLL_DESIRED);
		if (err) {
			LOG_ERR("cloud_configuration_poll, error: %d", err);
			send_request_failed();
		}
		break;
	case CLOUD_SHADOW_SET_REPORTED_CONFIG:
		err = cloud_configuration_reported_set(msg->payload.buffer,
							  msg->payload.buffer_data_len);
		if (err) {
			LOG_ERR("cloud_configuration_reported_set, error: %d", err);
			send_request_failed();
		}
		break;
	case CLOUD_SHADOW_UPDATE_REPORTED_CONFIG:
		err = cloud_configuration_reported_update(msg->payload.buffer,
							  msg->payload.buffer_data_len);
		if (err) {
			LOG_ERR("cloud_configuration_reported_update, error: %d", err);
			send_request_failed();
		}
		break;
	case CLOUD_SHADOW_UPDATE_REPORTED_DEVICE:
		err = nrf_cloud_coap_shadow_configured_info_update(APP_VERSION_STRING);
		if (err) {
			LOG_ERR("nrf_cloud_coap_shadow_configured_info_update, error: %d", err);
			send_request_failed();
		}
		break;
	case CLOUD_PROVISIONING_REQUEST:
		LOG_DBG("Provisioning request received");
		smf_set_state(SMF_CTX(state_object), &states[STATE_PROVISIONING]);
		break;
	default:
		break;
	}
}

static void handle_priv_cloud_message(struct cloud_state_object const *state_object)
{
	const struct priv_cloud_msg *msg = (const struct priv_cloud_msg *)state_object->msg_buf;

	if (msg->type == CLOUD_SEND_REQUEST_FAILED) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTING]);
	}
}

static void handle_storage_data_message(struct cloud_state_object const *state_object)
{
	const struct storage_msg *msg = (const struct storage_msg *)state_object->msg_buf;

	if (msg->type == STORAGE_DATA) {
		LOG_DBG("Storage data received, type: %d, size: %d",
			msg->data_type, msg->data_len);
		handle_storage_data(msg);
	}
}

static void network_connection_status_retain(struct cloud_state_object *state_object)
{
	if (state_object->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		if (msg->type == NETWORK_DISCONNECTED || msg->type == NETWORK_CONNECTED) {
			/* Update network status to retain the last connection status */
			state_object->network_connected =
				(msg->type == NETWORK_CONNECTED) ? true : false;
		}
	}
}

static void shadow_network_info_update(void)
{
	int err = nrf_cloud_coap_shadow_network_info_update();

	if (err) {
		LOG_ERR("nrf_cloud_coap_shadow_network_info_update, error: %d", err);
	}
}

static enum batch_drain_result
handle_storage_batch_item(const struct cloud_state_object *state_object)
{
	int err;
	struct storage_data_item item;

	if (state_object->storage_batch_session_id == 0U) {
		return BATCH_DRAIN_ABORTED;
	}

	err = storage_batch_read(&item,
				 K_MSEC(CONFIG_APP_CLOUD_STORAGE_BATCH_ITEM_READ_TIMEOUT_MS));
	if (err == -EAGAIN) {
		/* No data became available within timeout; retry in next batch-item event. */
		storage_batch_item_process_trigger();
		return BATCH_DRAIN_CONTINUE;
	}

	if (err == -ENODATA) {
		LOG_DBG("Storage batch session 0x%X finished",
			state_object->storage_batch_session_id);
		return BATCH_DRAIN_DONE;
	}

	if (err) {
		LOG_ERR("storage_batch_read failed, error: %d", err);
		return BATCH_DRAIN_ABORTED;
	}

	err = send_storage_data_to_cloud(&item);
	if (err) {
		if (err == -ENOTSUP || err == -EINVAL) {
			LOG_ERR("Data error sending data (type %d): %d", item.type, err);
		} else {
			LOG_WRN("Network error sending data (type %d): %d", item.type, err);

			return BATCH_DRAIN_ABORTED;
		}
	}

	/* Consume the item after successful send or known malformed payload. */
	storage_batch_consume_item(state_object->storage_batch_session_id, item.type);
	storage_batch_item_process_trigger();

	return BATCH_DRAIN_CONTINUE;
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

	err = cloud_provisioning_init();
	if (err) {
		LOG_ERR("nrf_provisioning_init, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

/* Default handling of storage batch responses. Applies whenever the module is not able to
 * send the batch to cloud, in which case the batch session is closed right away.
 * Substates that can send data handle these messages themselves.
 */
static enum smf_state_result state_running_run(void *obj)
{
	struct cloud_state_object const *state_object = obj;

	if (state_object->chan == &storage_chan) {
		const struct storage_msg *msg = (const struct storage_msg *)state_object->msg_buf;

		switch (msg->type) {
		case STORAGE_BATCH_AVAILABLE:
		case STORAGE_BATCH_EMPTY:
			LOG_WRN("Storage batch received, cloud cannot send, closing session 0x%X",
				msg->session_id);

			handle_storage_batch_empty(msg);

			return SMF_EVENT_HANDLED;
		case STORAGE_BATCH_ERROR:
			LOG_DBG("Storage batch error received, closing session 0x%X",
				msg->session_id);

			handle_storage_batch_error(msg);

			return SMF_EVENT_HANDLED;
		case STORAGE_BATCH_BUSY:
			handle_storage_batch_busy(msg);

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_disconnected_entry(void *obj)
{
	int err;
	const struct cloud_msg cloud_msg = {
		.type = CLOUD_DISCONNECTED,
	};

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	err = zbus_chan_pub(&cloud_chan, &cloud_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static enum smf_state_result state_disconnected_run(void *obj)
{
	struct cloud_state_object *state_object = obj;

	if (state_object->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		if (msg->type == NETWORK_CONNECTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTING]);

			return SMF_EVENT_HANDLED;
		}
	}

#if defined(CONFIG_NRF_CLOUD_AGNSS) && defined(CONFIG_APP_LOCATION)
	if (state_object->chan == &location_chan) {
		const struct location_msg *msg = (const struct location_msg *)state_object->msg_buf;

		if (msg->type == LOCATION_AGNSS_REQUEST) {
			cloud_location_agnss_cache(msg);

			return SMF_EVENT_HANDLED;
		}
	}
#endif /* CONFIG_NRF_CLOUD_AGNSS && CONFIG_APP_LOCATION */

	return SMF_EVENT_PROPAGATE;
}

static void state_connecting_entry(void *obj)
{
	/* Reset connection attempts counter */
	struct cloud_state_object *state_object = obj;

	LOG_DBG("%s", __func__);

	state_object->connection_attempts = 0;
	state_object->provisioning_ongoing = false;
}

static enum smf_state_result state_connecting_run(void *obj)
{
	struct cloud_state_object *state_object = obj;

	if (state_object->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		if (msg->type == NETWORK_DISCONNECTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED]);

			return SMF_EVENT_HANDLED;
		}
	}

#if defined(CONFIG_NRF_CLOUD_AGNSS) && defined(CONFIG_APP_LOCATION)
	if (state_object->chan == &location_chan) {
		const struct location_msg *msg = (const struct location_msg *)state_object->msg_buf;

		if (msg->type == LOCATION_AGNSS_REQUEST) {
			cloud_location_agnss_cache(msg);

			return SMF_EVENT_HANDLED;
		}
	}
#endif /* CONFIG_NRF_CLOUD_AGNSS && CONFIG_APP_LOCATION */

	return SMF_EVENT_PROPAGATE;
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

static enum smf_state_result state_connecting_provisioned_run(void *obj)
{
	struct cloud_state_object *state_object = obj;

	if (state_object->chan == &priv_cloud_chan) {
		const struct priv_cloud_msg *msg =
			(const struct priv_cloud_msg *)state_object->msg_buf;

		if (msg->type == CLOUD_NOT_AUTHENTICATED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_PROVISIONING]);

			return SMF_EVENT_HANDLED;
		} else if (msg->type == CLOUD_CONNECTION_SUCCESS) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED]);

			return SMF_EVENT_HANDLED;
		} else if (msg->type == CLOUD_CONNECTION_FAILED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTING_BACKOFF]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_connecting_provisioning_entry(void *obj)
{
	int err;
	struct cloud_state_object *state_object = obj;
	struct location_msg location_msg = {
		.type = LOCATION_SEARCH_CANCEL,
	};

	LOG_DBG("%s", __func__);

	/* Cancel any ongoing location search during provisioning to allow writing credentials,
	 * which requires offline LTE functional mode.
	 */
	err = zbus_chan_pub(&location_chan, &location_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	state_object->provisioning_ongoing = true;

	err = cloud_provisioning_trigger();
	if (err) {
		LOG_ERR("nrf_provisioning_trigger_manually, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static enum smf_state_result state_connecting_provisioning_run(void *obj)
{
	struct cloud_state_object *state_object = obj;

	if (state_object->chan == &priv_cloud_chan) {
		const struct priv_cloud_msg *msg =
			(const struct priv_cloud_msg *)state_object->msg_buf;

		if (msg->type == CLOUD_PROVISIONING_FINISHED) {
			send_provisioned_msg();
			smf_set_state(SMF_CTX(state_object), &states[STATE_PROVISIONED]);

			return SMF_EVENT_HANDLED;
		} else if (msg->type == CLOUD_PROVISIONING_FAILED &&
			   state_object->network_connected) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTING_BACKOFF]);

			return SMF_EVENT_HANDLED;
		} else if (msg->type == CLOUD_PROVISIONING_FAILED &&
			   !state_object->network_connected) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED]);

			return SMF_EVENT_HANDLED;
		}
	}

	/* Its expected that the device goes online/offline a few times during provisioning.
	 * Therefore we handle network connected/disconnected events in this state preventing it
	 * from propagating up the state machine changing the cloud module's connectivity status.
	 */
	if (state_object->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		if (msg->type == NETWORK_DISCONNECTED || msg->type == NETWORK_CONNECTED) {
			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
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

static enum smf_state_result state_connecting_backoff_run(void *obj)
{
	struct cloud_state_object const *state_object = obj;

	if (state_object->chan == &priv_cloud_chan) {
		const struct priv_cloud_msg *msg =
			(const struct priv_cloud_msg *)state_object->msg_buf;

		/* If the backoff timer expired, we can either continue provisioning or
		 * connect to cloud if already provisioned. The provisioning ongoing flag helps us
		 * determine what substate of connecting attempt we are attempting to enter.
		 */
		if ((msg->type == CLOUD_BACKOFF_EXPIRED) && !state_object->provisioning_ongoing) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_PROVISIONED]);

			return SMF_EVENT_HANDLED;
		} else if ((msg->type == CLOUD_BACKOFF_EXPIRED) &&
			   state_object->provisioning_ongoing) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_PROVISIONING]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
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

	err = zbus_chan_pub(&cloud_chan, &cloud_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static void state_connected_ready_entry(void *obj)
{
	int err;
	struct cloud_msg cloud_msg = {
		.type = CLOUD_CONNECTED,
	};

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	err = zbus_chan_pub(&cloud_chan, &cloud_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

#if defined(CONFIG_NRF_CLOUD_AGNSS) && defined(CONFIG_APP_LOCATION)
	cloud_location_agnss_process_cached();
#endif /* CONFIG_NRF_CLOUD_AGNSS && CONFIG_APP_LOCATION */
}

static enum smf_state_result state_connected_ready_run(void *obj)
{
	struct cloud_state_object *state_object = obj;

	if (state_object->chan == &priv_cloud_chan) {
		handle_priv_cloud_message(state_object);
		return SMF_EVENT_HANDLED;
	}

	if (state_object->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		switch (msg->type) {
		case NETWORK_DISCONNECTED:
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_PAUSED]);

			return SMF_EVENT_HANDLED;
		case NETWORK_CONNECTED:
			return SMF_EVENT_HANDLED;
		default:
			break;
		}

		return SMF_EVENT_HANDLED;
	}

	if (state_object->chan == &storage_data_chan) {
		handle_storage_data_message(state_object);

		return SMF_EVENT_HANDLED;
	}

	if (state_object->chan == &cloud_chan) {
		handle_cloud_channel_message(state_object);

		return SMF_EVENT_HANDLED;
	}

#if defined(CONFIG_APP_LOCATION)
	if (state_object->chan == &location_chan) {
		const struct location_msg *msg = (const struct location_msg *)state_object->msg_buf;

		if (msg->type == LOCATION_AGNSS_REQUEST) {
			LOG_DBG("A-GNSS data request received");

			(void)cloud_location_handle_message(msg);
		}

		return SMF_EVENT_HANDLED;
	}
#endif /* CONFIG_APP_LOCATION */

	return SMF_EVENT_PROPAGATE;
}

static enum smf_state_result state_connected_idle_run(void *obj)
{
	struct cloud_state_object *state_object = obj;

	if (state_object->chan == &storage_chan) {
		const struct storage_msg *msg = (const struct storage_msg *)state_object->msg_buf;

		switch (msg->type) {
		case STORAGE_BATCH_AVAILABLE:
			LOG_DBG("Storage batch available, %d items, session_id: 0x%X",
				msg->data_len, msg->session_id);
			state_object->storage_batch_session_id = msg->session_id;
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_SENDING_DATA]);
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
		return SMF_EVENT_HANDLED;
	}
	return SMF_EVENT_PROPAGATE;
}

static void state_connected_sending_data_entry(void *obj)
{
	struct cloud_state_object *state_object = obj;

	LOG_DBG("%s", __func__);

	if (state_object->storage_batch_session_id == 0U) {
		LOG_WRN("Entered sending state without active batch, returning to ready");
		smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_IDLE]);
		return;
	}

	storage_batch_item_process_trigger();
}

static enum smf_state_result
handle_sending_data_storage_msg(struct cloud_state_object *state_object)
{
	const struct storage_msg *msg = (const struct storage_msg *)state_object->msg_buf;
	bool session_matches = (state_object->storage_batch_session_id != 0U &&
				msg->session_id == state_object->storage_batch_session_id);

	switch (msg->type) {
	case STORAGE_BATCH_ERROR:
		if (!session_matches) {
			break;
		}
		LOG_ERR("Storage batch error while sending, session_id: 0x%X", msg->session_id);
		smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_IDLE]);
		return SMF_EVENT_HANDLED;
	case STORAGE_BATCH_EMPTY:
		if (!session_matches) {
			break;
		}
		smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_IDLE]);
		return SMF_EVENT_HANDLED;
	default:
		break;
	}

	return SMF_EVENT_PROPAGATE;
}

static enum smf_state_result state_connected_sending_data_run(void *obj)
{
	struct cloud_state_object *state_object = obj;

	if (state_object->chan == &priv_cloud_chan) {
		const struct priv_cloud_msg *msg =
			(const struct priv_cloud_msg *)state_object->msg_buf;

		if (msg->type == CLOUD_STORAGE_BATCH_ITEM_PROCESS) {
			switch (handle_storage_batch_item(state_object)) {
			case BATCH_DRAIN_DONE:
				shadow_network_info_update();
				smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_IDLE]);
				break;

			case BATCH_DRAIN_ABORTED:
				smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_IDLE]);
				break;

			case BATCH_DRAIN_CONTINUE:
				break;
			}
			return SMF_EVENT_HANDLED;
		}

		handle_priv_cloud_message(state_object);
		return SMF_EVENT_HANDLED;
	}

	if (state_object->chan == &storage_chan) {
		return handle_sending_data_storage_msg(state_object);
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_connected_sending_data_exit(void *obj)
{
	struct cloud_state_object *state_object = obj;

	LOG_DBG("%s", __func__);

	storage_batch_close(state_object);
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

	err = zbus_chan_pub(&cloud_chan, &cloud_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static enum smf_state_result state_connected_paused_run(void *obj)
{
	struct cloud_state_object const *state_object = obj;

	if (state_object->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		if (msg->type == NETWORK_CONNECTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_READY]);

			return SMF_EVENT_HANDLED;
		}
	}

#if defined(CONFIG_NRF_CLOUD_AGNSS) && defined(CONFIG_APP_LOCATION)
	if (state_object->chan == &location_chan) {
		const struct location_msg *msg = (const struct location_msg *)state_object->msg_buf;

		if (msg->type == LOCATION_AGNSS_REQUEST) {
			cloud_location_agnss_cache(msg);

			return SMF_EVENT_HANDLED;
		}
	}
#endif /* CONFIG_NRF_CLOUD_AGNSS && CONFIG_APP_LOCATION */

	return SMF_EVENT_PROPAGATE;
}

static void cloud_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = (CONFIG_APP_CLOUD_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_CLOUD_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	/* Place state object in .data section to ensure it is captured in coredumps
	 * and can be inspected by external tools during state analysis.
	 */
	__attribute__((section(".data"))) static struct cloud_state_object cloud_state = {
		.backoff_time = CONFIG_APP_CLOUD_BACKOFF_INITIAL_SECONDS,
	};

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

		network_connection_status_retain(&cloud_state);

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
