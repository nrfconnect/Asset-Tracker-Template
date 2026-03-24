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
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/net_l2.h>
#include <zephyr/net/net_if.h>

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

/* State machine */

enum network_module_state {
	/* The module is running */
	STATE_RUNNING,
		/* The device is not connected to a network */
		STATE_DISCONNECTED,
			/* The device is disconnected from network and is not searching */
			STATE_DISCONNECTED_IDLE,
			/* The device is disconnected and attempting to connect */
			STATE_DISCONNECTED_SEARCHING,
		/* The device is connected to a network */
		STATE_CONNECTED,
		/* The device has initiated disconnection */
		STATE_DISCONNECTING,
};

struct network_state_object {
	/* This must be first */
	struct smf_ctx ctx;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;

	/* Buffer for last ZBus message */
	uint8_t msg_buf[MAX_MSG_SIZE];
};

/* Forward declarations of state handlers */
static void state_running_entry(void *obj);
static enum smf_state_result state_running_run(void *obj);
static void state_disconnected_entry(void *obj);
static enum smf_state_result state_disconnected_run(void *obj);
static enum smf_state_result state_disconnected_idle_run(void *obj);
static void state_disconnected_searching_entry(void *obj);
static enum smf_state_result state_disconnected_searching_run(void *obj);
static void state_disconnecting_entry(void *obj);
static enum smf_state_result state_disconnecting_run(void *obj);
static enum smf_state_result state_connected_run(void *obj);
static void state_connected_entry(void *obj);

/* State machine definition */
static const struct smf_state states[] = {
	[STATE_RUNNING] =
		SMF_CREATE_STATE(state_running_entry, state_running_run, NULL,
				 NULL,	/* No parent state */
				 &states[STATE_DISCONNECTED]),
#if defined(CONFIG_APP_NETWORK_SEARCH_NETWORK_ON_STARTUP)
	[STATE_DISCONNECTED] =
		SMF_CREATE_STATE(state_disconnected_entry, state_disconnected_run, NULL,
				 &states[STATE_RUNNING],
				 &states[STATE_DISCONNECTED_SEARCHING]),
#else
	[STATE_DISCONNECTED] =
		SMF_CREATE_STATE(state_disconnected_entry, state_disconnected_run, NULL,
				 &states[STATE_RUNNING],
				 &states[STATE_DISCONNECTED_IDLE]),
#endif /* CONFIG_APP_NETWORK_SEARCH_NETWORK_ON_STARTUP */
	[STATE_DISCONNECTED_IDLE] =
		SMF_CREATE_STATE(NULL, state_disconnected_idle_run, NULL,
				 &states[STATE_DISCONNECTED],
				 NULL), /* No initial transition */
	[STATE_DISCONNECTED_SEARCHING] =
		SMF_CREATE_STATE(state_disconnected_searching_entry,
				 state_disconnected_searching_run, NULL,
				 &states[STATE_DISCONNECTED],
				 NULL), /* No initial transition */
	[STATE_CONNECTED] =
		SMF_CREATE_STATE(state_connected_entry, state_connected_run, NULL,
				 &states[STATE_RUNNING],
				 NULL), /* No initial transition */
	[STATE_DISCONNECTING] =
		SMF_CREATE_STATE(state_disconnecting_entry, state_disconnecting_run, NULL,
				 &states[STATE_RUNNING],
				 NULL), /* No initial transition */
};

/* --- Connection Manager event handling --- */

static struct net_mgmt_event_callback l4_cb;
static struct net_if *ppp_iface;

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

static void l4_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
			     struct net_if *iface)
{
	if (event == NET_EVENT_L4_CONNECTED) {
		LOG_INF("Network connectivity established (L4)");
		network_status_notify(NETWORK_CONNECTED);
	} else if (event == NET_EVENT_L4_DISCONNECTED) {
		LOG_INF("Network connectivity lost (L4)");
		network_status_notify(NETWORK_DISCONNECTED);
	}
}

static int network_connect(void)
{
	if (ppp_iface == NULL) {
		LOG_ERR("PPP network interface not found");
		return -ENODEV;
	}

	return net_if_up(ppp_iface);
}

static int network_disconnect(void)
{
	if (ppp_iface == NULL) {
		LOG_ERR("PPP network interface not found");
		return -ENODEV;
	}

	return net_if_down(ppp_iface);
}

/* --- State handlers --- */

static void state_running_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("state_running_entry");

	ppp_iface = net_if_get_first_by_type(&NET_L2_GET_NAME(PPP));
	if (ppp_iface == NULL) {
		LOG_ERR("PPP network interface not found");
		SEND_FATAL_ERROR();
		return;
	}

	/* Register for L4 connectivity events from conn_mgr */
	net_mgmt_init_event_callback(&l4_cb, l4_event_handler,
				     NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED);
	net_mgmt_add_event_callback(&l4_cb);

	/* Request conn_mgr to re-send current status in case we missed the initial event */
	conn_mgr_mon_resend_status();

	LOG_DBG("Network PPP module started");
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
		case NETWORK_DISCONNECTED:
			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_disconnected_searching_entry(void *obj)
{
	int err;

	ARG_UNUSED(obj);

	LOG_DBG("state_disconnected_searching_entry");

	err = network_connect();
	if (err) {
		LOG_ERR("network_connect, error: %d", err);
	}
}

static enum smf_state_result state_disconnected_searching_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	if (&network_chan == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_CONNECT:
			return SMF_EVENT_HANDLED;
		case NETWORK_SEARCH_STOP:
			__fallthrough;
		case NETWORK_DISCONNECT: {
			int err = network_disconnect();

			if (err) {
				LOG_ERR("network_disconnect, error: %d", err);
				SEND_FATAL_ERROR();
			}

			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);
			return SMF_EVENT_HANDLED;
		}
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static enum smf_state_result state_disconnected_idle_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	if (&network_chan == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_DISCONNECT:
			return SMF_EVENT_HANDLED;
		case NETWORK_CONNECT:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_SEARCHING]);
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

		if (msg.type == NETWORK_DISCONNECT) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTING]);
			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_disconnecting_entry(void *obj)
{
	int err;

	ARG_UNUSED(obj);

	LOG_DBG("state_disconnecting_entry");

	err = network_disconnect();
	if (err) {
		LOG_ERR("network_disconnect, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static enum smf_state_result state_disconnecting_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	if (&network_chan == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		if (msg.type == NETWORK_DISCONNECTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);
			return SMF_EVENT_HANDLED;
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
