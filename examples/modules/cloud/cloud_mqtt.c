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
#include <net/mqtt_helper.h>
#include <modem/nrf_modem_lib.h>
#include <modem/modem_key_mgmt.h>
#include <hw_id.h>

#include "cloud.h"
#include "network.h"
#include "app_common.h"

/* Define FOTA and Location channels to avoid build warning due to the module being patched out
 * because they are not supported with the MQTT cloud.
 */
#include "fota.h"
#include "location.h"

ZBUS_CHAN_DEFINE(FOTA_CHAN,
		 enum fota_msg_type,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

ZBUS_CHAN_DEFINE(LOCATION_CHAN_MQTT,
		 enum location_msg_type,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Register log module */
LOG_MODULE_REGISTER(cloud, CONFIG_APP_CLOUD_MQTT_LOG_LEVEL);

BUILD_ASSERT(CONFIG_APP_CLOUD_MQTT_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_CLOUD_MQTT_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

static const unsigned char ca_certificate[] = {
	#include ATT_MQTT_CA_CERT
	(0x00)
};

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(cloud_subscriber);

#define CHANNEL_LIST(X)						\
		X(NETWORK_CHAN,	struct network_msg)		\
		X(CLOUD_CHAN, struct cloud_msg)

/* Calculate the maximum message size from the list of channels */
#define MAX_MSG_SIZE			MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

/* Add the cloud_subscriber as observer to all the channels in the list. */
#define ADD_OBSERVERS(_chan, _type)	ZBUS_CHAN_ADD_OBS(_chan, cloud_subscriber, 0);

CHANNEL_LIST(ADD_OBSERVERS)

#define SUBSCRIBE_TOPIC_ID 2469

/* Define channels provided by this module */
ZBUS_CHAN_DEFINE(CLOUD_CHAN,
		 struct cloud_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.type = CLOUD_DISCONNECTED)
);

static void on_modem_init(int ret, void *ctx)
{
	ARG_UNUSED(ctx);

	if (ret) {
		LOG_ERR("Modem init failed, ignoring CA provisioning, : %d", ret);
		return;
	}

	int err = modem_key_mgmt_write(CONFIG_APP_CLOUD_MQTT_SEC_TAG,
				       MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
				       ca_certificate,
				       sizeof(ca_certificate));
	if (err < 0) {
		LOG_ERR("Failed to write credential: %d", err);
		return;
	}
}

NRF_MODEM_LIB_ON_INIT(att_cloud_mqtt_hook, on_modem_init, NULL);

/* Enumerator to be used in privat cloud channel */
enum priv_cloud_msg {
	CLOUD_CONNECTION_ATTEMPTED,
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
static void state_connecting_run(void *o);

static void state_connecting_attempt_entry(void *o);
static void state_connecting_attempt_run(void *o);

static void state_connecting_backoff_entry(void *o);
static void state_connecting_backoff_run(void *o);
static void state_connecting_backoff_exit(void *o);

static void state_connected_entry(void *o);
static void state_connected_run(void *o);
static void state_connected_exit(void *o);

enum cloud_module_state {
	/* Normal operation */
	STATE_RUNNING,
		/* Disconnected from the cloud */
		STATE_DISCONNECTED,
		/* Connecting to cloud */
		STATE_CONNECTING,
			/* Attempting to connect to cloud */
			STATE_CONNECTING_ATTEMPT,
			/* Waiting before trying to connect again */
			STATE_CONNECTING_BACKOFF,
		/* Connected to cloud */
		STATE_CONNECTED,
};

/* Construct state table */
static const struct smf_state states[] = {
	[STATE_RUNNING] =
		SMF_CREATE_STATE(state_running_entry, state_running_run, NULL,
				 NULL,			       /* No parent state */
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
		SMF_CREATE_STATE(state_connecting_attempt_entry, state_connecting_attempt_run, NULL,
				 &states[STATE_CONNECTING],
				 NULL),

	[STATE_CONNECTING_BACKOFF] =
		SMF_CREATE_STATE(state_connecting_backoff_entry, state_connecting_backoff_run,
				 state_connecting_backoff_exit,
				 &states[STATE_CONNECTING],
				 NULL),

	[STATE_CONNECTED] =
		SMF_CREATE_STATE(state_connected_entry, state_connected_run, state_connected_exit,
				 &states[STATE_RUNNING],
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

	/* Topics that are published and subscribed to */
	char pub_topic[CONFIG_APP_CLOUD_MQTT_TOPIC_SIZE_MAX];
	char sub_topic[CONFIG_APP_CLOUD_MQTT_TOPIC_SIZE_MAX];

	/* MQTT client ID */
	char client_id[HW_ID_LEN];

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

static int topics_prefix(char *pub_topic, size_t pub_topic_len,
			 char *sub_topic, size_t sub_topic_len,
			 char *client_id)
{
	int len;

	len = snprintk(pub_topic, pub_topic_len, "%s/%s", client_id,
		       CONFIG_APP_CLOUD_MQTT_PUB_TOPIC);
	if ((len < 0) || (len >= pub_topic_len)) {
		LOG_ERR("Publish topic buffer too small, len: %d, max: %d",
			len, pub_topic_len);
		return -EMSGSIZE;
	}

	len = snprintk(sub_topic, sub_topic_len, "%s/%s", client_id,
		       CONFIG_APP_CLOUD_MQTT_SUB_TOPIC);
	if ((len < 0) || (len >= sub_topic_len)) {
		LOG_ERR("Subscribe topic buffer too small, len: %d, max: %d",
			len, sub_topic_len);
		return -EMSGSIZE;
	}

	return 0;
}

static void connect_to_cloud(const struct cloud_state *state_object)
{
	int err;
	struct cloud_state *object = (struct cloud_state *)state_object;
	enum priv_cloud_msg msg = CLOUD_CONNECTION_ATTEMPTED;
	struct mqtt_helper_conn_params conn_params = {
		.hostname.ptr = CONFIG_APP_CLOUD_MQTT_HOSTNAME,
		.hostname.size = strlen(CONFIG_APP_CLOUD_MQTT_HOSTNAME),
		.device_id.ptr = object->client_id,
		.device_id.size = strlen(object->client_id),
	};

	err = hw_id_get(object->client_id, sizeof(object->client_id));
	if (err) {
		LOG_ERR("hw_id_get, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	/* Prefix topics with the client ID so that the topics are unique per device.
	 * This mitigates conflict between similar clients subscribing/publishing to the same
	 * topics.
	 */
	err = topics_prefix(object->pub_topic,
			    sizeof(object->pub_topic),
			    object->sub_topic,
			    sizeof(object->sub_topic),
			    object->client_id);
	if (err) {
		LOG_ERR("topics_prefix, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	err = mqtt_helper_connect(&conn_params);
	if (err) {
		LOG_ERR("Failed connecting to MQTT, error code: %d", err);
	}

	err = zbus_chan_pub(&PRIV_CLOUD_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static uint32_t calculate_backoff_time(uint32_t attempts)
{
	uint32_t backoff_time = CONFIG_APP_CLOUD_MQTT_BACKOFF_INITIAL_SECONDS;

	/* Calculate backoff time */
	if (IS_ENABLED(CONFIG_APP_CLOUD_MQTT_BACKOFF_TYPE_EXPONENTIAL)) {
		backoff_time = CONFIG_APP_CLOUD_MQTT_BACKOFF_INITIAL_SECONDS << (attempts - 1);
	} else if (IS_ENABLED(CONFIG_APP_CLOUD_MQTT_BACKOFF_TYPE_LINEAR)) {
		backoff_time = CONFIG_APP_CLOUD_MQTT_BACKOFF_INITIAL_SECONDS +
			((attempts - 1) * CONFIG_APP_CLOUD_MQTT_BACKOFF_LINEAR_INCREMENT_SECONDS);
	}

	/* Limit backoff time */
	if (backoff_time > CONFIG_APP_CLOUD_MQTT_BACKOFF_MAX_SECONDS) {
		backoff_time = CONFIG_APP_CLOUD_MQTT_BACKOFF_MAX_SECONDS;
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

static void on_cloud_payload_json(const struct cloud_msg *msg,
				  const struct cloud_state *state_object)
{
	int err;
	struct mqtt_publish_param param = {
		.message.payload.data = (uint8_t *)msg->payload.buffer,
		.message.payload.len = msg->payload.buffer_data_len,
		.message.topic.qos = MQTT_QOS_1_AT_LEAST_ONCE,
		.message_id = mqtt_helper_msg_id_get(),
		.message.topic.topic.utf8 = state_object->pub_topic,
		.message.topic.topic.size = strlen(state_object->pub_topic),
	};

	LOG_DBG("MQTT Publish Details:");
	LOG_DBG("\t-Payload: %.*s", param.message.payload.len, param.message.payload.data);
	LOG_DBG("\t-Payload Length: %d", param.message.payload.len);
	LOG_DBG("\t-Topic: %.*s", param.message.topic.topic.size, param.message.topic.topic.utf8);
	LOG_DBG("\t-Topic Size: %d", param.message.topic.topic.size);
	LOG_DBG("\t-QoS: %d", param.message.topic.qos);
	LOG_DBG("\t-Message ID: %d", param.message_id);

	err = mqtt_helper_publish(&param);
	if (err) {
		LOG_ERR("mqtt_helper_publish, error: %d", err);
	}
}

static void on_mqtt_connack(enum mqtt_conn_return_code return_code, bool session_present)
{
	int err;
	struct cloud_msg cloud_msg = {
		.type = CLOUD_CONNECTED,
	};

	LOG_DBG("MQTT connection established, session present: %d", session_present);

	if (return_code != MQTT_CONNECTION_ACCEPTED) {
		LOG_ERR("Failed connecting to MQTT, error code: %d", return_code);
		return;
	}

	err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void on_mqtt_disconnect(int result)
{
	ARG_UNUSED(result);

	int err;
	struct cloud_msg cloud_msg = {
		.type = CLOUD_DISCONNECTED,
	};

	err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void on_mqtt_publish(struct mqtt_helper_buf topic, struct mqtt_helper_buf payload)
{
	LOG_DBG("Received payload: %.*s on topic: %.*s", payload.size,
							 payload.ptr,
							 topic.size,
							 topic.ptr);
}

static void on_mqtt_suback(uint16_t message_id, int result)
{
	if ((message_id == SUBSCRIBE_TOPIC_ID) && (result == 0)) {
		LOG_DBG("Subscribed to topic with id: %d", message_id);
	} else if (result) {
		LOG_ERR("Topic subscription failed, error: %d", result);
	} else {
		LOG_WRN("Subscribed to unknown topic, id: %d", message_id);
	}
}

static void on_mqtt_puback(uint16_t message_id, int result)
{
	if (result) {
		LOG_ERR("Publish failed, error: %d", result);
	} else {
		LOG_DBG("Publish acknowledgment received, message id: %d", message_id);
	}
}

/* Zephyr State Machine Framework handlers */

/* Handler for STATE_RUNNING */

static void state_running_entry(void *o)
{
	int err;

	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	struct mqtt_helper_cfg cfg = {
		.cb = {
			.on_connack = on_mqtt_connack,
			.on_disconnect = on_mqtt_disconnect,
			.on_publish = on_mqtt_publish,
			.on_suback = on_mqtt_suback,
			.on_puback = on_mqtt_puback,
		},
	};

	err = mqtt_helper_init(&cfg);
	if (err) {
		LOG_ERR("mqtt_helper_init, error: %d", err);
		SEND_FATAL_ERROR();
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

/* Handlers for STATE_DISCONNECTED */

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

static void state_connecting_run(void *o)
{
	const struct cloud_state *state_object = (const struct cloud_state *)o;

	if (state_object->chan == &CLOUD_CHAN) {
		const struct cloud_msg *msg = MSG_TO_CLOUD_MSG_PTR(state_object->msg_buf);

		if (msg->type == CLOUD_CONNECTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED]);
		}
	}
}

/* Handler for STATE_CONNECTING_ATTEMPT */

static void state_connecting_attempt_entry(void *o)
{
	struct cloud_state *state_object = o;

	LOG_DBG("%s", __func__);

	state_object->connection_attempts++;

	connect_to_cloud(state_object);
}

static void state_connecting_attempt_run(void *o)
{
	struct cloud_state *state_object = o;

	LOG_DBG("%s", __func__);

	if (state_object->chan == &PRIV_CLOUD_CHAN) {
		const enum priv_cloud_msg msg = *(const enum priv_cloud_msg *)state_object->msg_buf;

		if (msg == CLOUD_CONNECTION_ATTEMPTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTING_BACKOFF]);

			return;
		}
	}
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

/* Handler for STATE_CLOUD_CONNECTED */

static void state_connected_entry(void *o)
{
	int err;
	struct cloud_state *state_object = (struct cloud_state *)o;
	struct mqtt_topic topics[] = {
		{
			.topic.utf8 = state_object->sub_topic,
			.topic.size = strlen(state_object->sub_topic),
		},
	};
	struct mqtt_subscription_list list = {
		.list = topics,
		.list_count = ARRAY_SIZE(topics),
		.message_id = SUBSCRIBE_TOPIC_ID,
	};

	LOG_DBG("%s", __func__);
	LOG_DBG("Connected to Cloud");

	for (size_t i = 0; i < list.list_count; i++) {
		LOG_INF("Subscribing to: %s", (char *)list.list[i].topic.utf8);
	}

	err = mqtt_helper_subscribe(&list);
	if (err) {
		LOG_ERR("Failed to subscribe to topics, error: %d", err);
		return;
	}
}

static void state_connected_run(void *o)
{
	const struct cloud_state *state_object = (const struct cloud_state *)o;

	if (state_object->chan == &CLOUD_CHAN) {
		const struct cloud_msg *msg = MSG_TO_CLOUD_MSG_PTR(state_object->msg_buf);

		if (msg->type == CLOUD_DISCONNECTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTING]);
		}

		if (msg->type == CLOUD_PAYLOAD_JSON) {
			on_cloud_payload_json(msg, state_object);
		}
	}
}

static void state_connected_exit(void *o)
{
	int err;

	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	err = mqtt_helper_disconnect();
	if (err) {
		LOG_ERR("Failed disconnecting from MQTT, error code: %d", err);
		LOG_ERR("This might occur if the connection is already closed");
	}
}

/* End of state handlers */

static void cloud_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms =
		(CONFIG_APP_CLOUD_MQTT_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_CLOUD_MQTT_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	struct cloud_state cloud_state = { 0 };

	LOG_DBG("Cloud MQTT module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, task_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("task_wdt_add, error: %d", task_wdt_id);
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
		CONFIG_APP_CLOUD_MQTT_THREAD_STACK_SIZE,
		cloud_module_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
