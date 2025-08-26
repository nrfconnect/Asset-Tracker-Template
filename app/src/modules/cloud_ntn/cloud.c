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
#include <zephyr/net/socket.h>
#include <errno.h>

#include "cloud.h"
#include "app_common.h"
#include "network.h"
#include "button.h"

/* Message conversion macros */
#define MSG_TO_CLOUD_MSG(_msg) (*(const struct cloud_msg *)_msg)
#define MSG_TO_BUTTON_MSG(_msg) (*(const uint8_t *)_msg)

/* Register log module */
LOG_MODULE_REGISTER(cloud, CONFIG_APP_CLOUD_LOG_LEVEL);

BUILD_ASSERT(CONFIG_APP_CLOUD_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_CLOUD_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

/* Declare channels */
ZBUS_CHAN_DECLARE(NETWORK_CHAN);
ZBUS_CHAN_DECLARE(BUTTON_CHAN);

/* Define channels provided by this module */
ZBUS_CHAN_DEFINE(CLOUD_CHAN,
		 struct cloud_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.type = CLOUD_DISCONNECTED)
);

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(cloud);

/* Observe channels */
ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, cloud, 0);
ZBUS_CHAN_ADD_OBS(CLOUD_CHAN, cloud, 0);
ZBUS_CHAN_ADD_OBS(BUTTON_CHAN, cloud, 0);

/* Calculate the maximum message size */
#define MAX_MSG_SIZE (MAX(sizeof(struct network_msg), \
                         MAX(sizeof(struct cloud_msg), sizeof(uint8_t))))

/* Socket configuration */
#define CLOUD_SERVER_ADDR CONFIG_APP_CLOUD_NTN_SERVER_ADDR
#define CLOUD_SERVER_PORT CONFIG_APP_CLOUD_NTN_SERVER_PORT
#define UDP_IP_HEADER_SIZE 28

/* Socket state */
static int sock_fd = -1;
static struct sockaddr_storage host_addr;

/* State machine */
enum cloud_module_state {
	STATE_RUNNING = 0,
	STATE_DISCONNECTED,
	STATE_CONNECTING,
	STATE_CONNECTED,
};

/* State object */
struct cloud_state_object {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	uint8_t msg_buf[MAX_MSG_SIZE];
};

/* Forward declarations of state handlers */
static void state_running_entry(void *obj);
static void state_running_run(void *obj);
static void state_disconnected_entry(void *obj);
static void state_disconnected_run(void *obj);
static void state_connecting_entry(void *obj);
static void state_connected_entry(void *obj);
static void state_connected_run(void *obj);

/* State machine definition */
static const struct smf_state states[] = {
	[STATE_RUNNING] = SMF_CREATE_STATE(
		state_running_entry, state_running_run, NULL,
		NULL,
		&states[STATE_DISCONNECTED]
	),
	[STATE_DISCONNECTED] = SMF_CREATE_STATE(
		state_disconnected_entry, state_disconnected_run, NULL,
		&states[STATE_RUNNING],
		NULL
	),
	[STATE_CONNECTING] = SMF_CREATE_STATE(
		state_connecting_entry, NULL, NULL,
		&states[STATE_RUNNING],
		NULL
	),
	[STATE_CONNECTED] = SMF_CREATE_STATE(
		state_connected_entry, state_connected_run, NULL,
		&states[STATE_RUNNING],
		NULL
	),
};

static int sock_open_and_connect(void)
{
	int err;
	struct sockaddr_in *server4 = ((struct sockaddr_in *)&host_addr);

	server4->sin_family = AF_INET;
	server4->sin_port = htons(CLOUD_SERVER_PORT);

        inet_pton(AF_INET, CLOUD_SERVER_ADDR, &server4->sin_addr);

	/* Create UDP socket */
	sock_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock_fd < 0) {
		LOG_ERR("Failed to create UDP socket, error: %d", errno);
		return -errno;
	}

	/* Connect socket */
	err = connect(sock_fd, (struct sockaddr *)&host_addr, sizeof(struct sockaddr_in));
	if (err < 0) {
		LOG_ERR("Failed to connect socket, error: %d", errno);
		close(sock_fd);
		sock_fd = -1;
		return -errno;
	}

	LOG_DBG("PIPPO UDP socket created and connected, fd=%d", sock_fd);
	return 0;
}

static int sock_send_data(void)
{
	int err;
	const char *message = "Hi from ATT via callbox";

	if (sock_fd < 0) {
		LOG_ERR("Socket not connected");
		return -ENOTCONN;
	}

	/* Send data */
	err = send(sock_fd, message, strlen(message), 0);
	if (err < 0) {
		LOG_ERR("Failed to send data, error: %d", errno);
		return -errno;
	}

	LOG_DBG("PIPPO Sent UDP/IP payload of %d bytes", strlen(message));
	return 0;
}

/* Cloud state handlers */

static void state_running_entry(void *obj)
{
	ARG_UNUSED(obj);
	LOG_DBG("%s", __func__);
}

static void state_running_run(void *obj)
{
	struct cloud_state_object const *state_object = obj;

	if (state_object->chan == &NETWORK_CHAN) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		if (msg.type == NETWORK_DISCONNECTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED]);
			return;
		}
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

	if (sock_fd >= 0) {
		close(sock_fd);
                LOG_WRN("Closed socket on disconnect");
		sock_fd = -1;
	}

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
	int err;

	LOG_DBG("%s", __func__);

	LOG_DBG("PIPPO state_connecting_entry");

	err = sock_open_and_connect();
	if (err) {
		LOG_ERR("Failed to connect to cloud server, error: %d", err);
		smf_set_state(SMF_CTX(obj), &states[STATE_DISCONNECTED]);
		return;
	}

	LOG_DBG("PIPPO sock_open_and_connect DONE");

	smf_set_state(SMF_CTX(obj), &states[STATE_CONNECTED]);
}

static void state_connected_entry(void *obj)
{
	int err;
	struct cloud_msg cloud_msg = {
		.type = CLOUD_CONNECTED,
	};

	ARG_UNUSED(obj);
	LOG_DBG("%s", __func__);

	LOG_DBG("PIPPO state_connected_entry");

	// err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_SECONDS(1));
	// if (err) {
	// 	LOG_ERR("zbus_chan_pub, error: %d", err);
	// 	SEND_FATAL_ERROR();
	// 	return;
	// }
}

static void state_connected_run(void *obj)
{
	struct cloud_state_object const *state_object = obj;

        LOG_DBG("PIPPO state_connected_run, push button to send");

	if (state_object->chan == &BUTTON_CHAN) {
		int err;

		/* Send data when button is pressed */
		err = sock_send_data();
		if (err) {
			LOG_ERR("Failed to send data, error: %d", err);
		}
	}
}

static void cloud_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

    SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
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

	/* Initialize the state machine */
	smf_set_initial(SMF_CTX(&cloud_state), &states[STATE_RUNNING]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = zbus_sub_wait_msg(&cloud, &cloud_state.chan,
				       cloud_state.msg_buf, zbus_wait_ms);
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
                cloud_module_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
