/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/smf.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/conn_mgr_monitor.h>

#include "app_common.h"
#include "network.h"

/* Register log module */
LOG_MODULE_REGISTER(network, CONFIG_APP_NETWORK_LOG_LEVEL);

BUILD_ASSERT(CONFIG_APP_NETWORK_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_NETWORK_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

/* Define channels provided by this module */
ZBUS_CHAN_DEFINE(network_chan,
		 struct network_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(network);

/* Observe network channel */
ZBUS_CHAN_ADD_OBS(network_chan, network, 0);

#define MAX_MSG_SIZE sizeof(struct network_msg)

/* Network module states */
enum network_module_state {
	STATE_RUNNING,
		STATE_DISCONNECTED,
		STATE_CONNECTED,
};

/* State object */
struct network_state_object {
	struct smf_ctx ctx;

	const struct zbus_channel *chan;

	uint8_t msg_buf[MAX_MSG_SIZE];
};

/* Forward declarations of state handlers */
static void state_running_entry(void *obj);
static enum smf_state_result state_running_run(void *obj);
static void state_disconnected_entry(void *obj);
static enum smf_state_result state_disconnected_run(void *obj);
static void state_connected_entry(void *obj);
static enum smf_state_result state_connected_run(void *obj);

/* State machine definition */
static const struct smf_state states[] = {
	[STATE_RUNNING] =
		SMF_CREATE_STATE(state_running_entry, state_running_run, NULL,
				 NULL,
				 &states[STATE_DISCONNECTED]),
	[STATE_DISCONNECTED] =
		SMF_CREATE_STATE(state_disconnected_entry, state_disconnected_run, NULL,
				 &states[STATE_RUNNING],
				 NULL),
	[STATE_CONNECTED] =
		SMF_CREATE_STATE(state_connected_entry, state_connected_run, NULL,
				 &states[STATE_RUNNING],
				 NULL),
};

static void network_status_notify(enum network_msg_type status)
{
	int err;
	struct network_msg msg = {
		.type = status,
	};

	err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

/* Connection manager event callback — runs in the net_mgmt workqueue context.
 * Posts events to the network channel so the state machine picks them up.
 */
static struct net_mgmt_event_callback l4_cb;

static void l4_event_handler(struct net_mgmt_event_callback *cb,
			      uint64_t event,
			      struct net_if *iface)
{
	switch (event) {
	case NET_EVENT_L4_CONNECTED:
		LOG_DBG("Network L4 connected");
		network_status_notify(NETWORK_CONNECTED);
		break;
	case NET_EVENT_L4_DISCONNECTED:
		LOG_DBG("Network L4 disconnected");
		network_status_notify(NETWORK_DISCONNECTED);
		break;
	default:
		break;
	}
}

/* State handlers */

static void state_running_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("state_running_entry");

	/* Register for L4 connectivity events */
	net_mgmt_init_event_callback(&l4_cb, l4_event_handler, (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED));
	net_mgmt_add_event_callback(&l4_cb);

	/* Request the connection manager to bring up all interfaces.
	 * The modem_cellular driver will handle CMUX, AT commands, PPP dial, and registration.
	 */
	conn_mgr_all_if_connect(true);

	LOG_DBG("Network module started, waiting for PPP connectivity");
}

static enum smf_state_result state_running_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	if (&network_chan == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_DISCONNECTED:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED]);
			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_disconnected_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("state_disconnected_entry");
}

static enum smf_state_result state_disconnected_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	if (&network_chan == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_CONNECTED:
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED]);
			return SMF_EVENT_HANDLED;
		case NETWORK_CONNECT:
			conn_mgr_all_if_connect(true);
			return SMF_EVENT_HANDLED;
		case NETWORK_DISCONNECT:
			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_connected_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("state_connected_entry");
}

static enum smf_state_result state_connected_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	if (&network_chan == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_DISCONNECT:
			conn_mgr_all_if_disconnect(true);
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED]);
			return SMF_EVENT_HANDLED;
		case NETWORK_DISCONNECTED:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED]);
			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void network_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Network watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void network_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms =
		(CONFIG_APP_NETWORK_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_NETWORK_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	static struct network_state_object network_state;

	task_wdt_id = task_wdt_add(wdt_timeout_ms, network_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		SEND_FATAL_ERROR();
		return;
	}

	smf_set_initial(SMF_CTX(&network_state), &states[STATE_RUNNING]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = zbus_sub_wait_msg(&network, &network_state.chan,
					network_state.msg_buf, zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = smf_run_state(SMF_CTX(&network_state));
		if (err) {
			LOG_ERR("smf_run_state(), error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}
	}
}

K_THREAD_DEFINE(network_module_thread_id,
		CONFIG_APP_NETWORK_THREAD_STACK_SIZE,
		network_module_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
