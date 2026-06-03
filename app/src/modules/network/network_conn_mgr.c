/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/smf.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/conn_mgr_monitor.h>

#include "app_common.h"
#ifdef CONFIG_APP_INSPECT_SHELL
#include "app_inspect.h"
#endif /* CONFIG_APP_INSPECT_SHELL */
#include "network.h"

LOG_MODULE_REGISTER(network, CONFIG_APP_NETWORK_LOG_LEVEL);

BUILD_ASSERT(CONFIG_APP_NETWORK_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_NETWORK_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

ZBUS_CHAN_DEFINE(network_chan,
		 struct network_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

ZBUS_MSG_SUBSCRIBER_DEFINE(network);
ZBUS_CHAN_ADD_OBS(network_chan, network, 0);

#define MAX_MSG_SIZE sizeof(struct network_msg)

#define L4_EVENT_MASK		(NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
#define CONN_LAYER_EVENT_MASK	(NET_EVENT_CONN_IF_FATAL_ERROR)

enum network_module_state {
	STATE_RUNNING,
		STATE_DISCONNECTED,
			STATE_DISCONNECTED_IDLE,
			STATE_DISCONNECTED_SEARCHING,
		STATE_CONNECTED,
		STATE_DISCONNECTING,
};

struct network_state_object {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	uint8_t msg_buf[MAX_MSG_SIZE];
};

static struct net_mgmt_event_callback l4_cb;
static struct net_mgmt_event_callback conn_cb;

static void state_running_entry(void *obj);
static enum smf_state_result state_running_run(void *obj);
static void state_disconnected_entry(void *obj);
static enum smf_state_result state_disconnected_run(void *obj);
static enum smf_state_result state_disconnected_idle_run(void *obj);
static void state_disconnected_searching_entry(void *obj);
static enum smf_state_result state_disconnected_searching_run(void *obj);
static void state_connected_entry(void *obj);
static enum smf_state_result state_connected_run(void *obj);
static void state_disconnecting_entry(void *obj);
static enum smf_state_result state_disconnecting_run(void *obj);

static const struct smf_state states[] = {
	[STATE_RUNNING] =
		SMF_CREATE_STATE(state_running_entry, state_running_run, NULL,
				 NULL,
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
				 NULL),
	[STATE_DISCONNECTED_SEARCHING] =
		SMF_CREATE_STATE(state_disconnected_searching_entry,
				 state_disconnected_searching_run, NULL,
				 &states[STATE_DISCONNECTED],
				 NULL),
	[STATE_CONNECTED] =
		SMF_CREATE_STATE(state_connected_entry, state_connected_run, NULL,
				 &states[STATE_RUNNING],
				 NULL),
	[STATE_DISCONNECTING] =
		SMF_CREATE_STATE(state_disconnecting_entry, state_disconnecting_run, NULL,
				 &states[STATE_RUNNING],
				 NULL),
};

#if defined(CONFIG_APP_INSPECT_SHELL)
static struct network_state_object *network_state_ctx;

static const char *network_state_to_string(enum network_module_state state)
{
	switch (state) {
	case STATE_RUNNING:			return "STATE_RUNNING";
	case STATE_DISCONNECTED:		return "STATE_DISCONNECTED";
	case STATE_DISCONNECTED_IDLE:		return "STATE_DISCONNECTED_IDLE";
	case STATE_DISCONNECTED_SEARCHING:	return "STATE_DISCONNECTED_SEARCHING";
	case STATE_CONNECTED:			return "STATE_CONNECTED";
	case STATE_DISCONNECTING:		return "STATE_DISCONNECTING";
	default:				return "STATE_UNKNOWN";
	}
}

APP_INSPECT_MODULE_REGISTER_STATE(network,
				  network_state_ctx,
				  states,
				  enum network_module_state,
				  network_state_to_string);
#endif /* CONFIG_APP_INSPECT_SHELL */

static void network_status_notify(enum network_msg_type status)
{
	int err;
	const struct network_msg msg = { .type = status };

	/* Use K_NO_WAIT: this is called from the net_mgmt event thread, and
	 * we don't want to block event processing if the channel is busy.
	 */
	err = zbus_chan_pub(&network_chan, &msg, K_NO_WAIT);
	if (err) {
		LOG_WRN("zbus_chan_pub(network_chan), error: %d", err);
	}
}

/* These callbacks run on the net_mgmt event thread. Logging via the
 * default deferred backend is cheap (just enqueues a log message), so it
 * is safe here. Avoid SEND_FATAL_ERROR()/LOG_PANIC()/k_sleep() in this
 * thread though - that would freeze net_mgmt event processing.
 */
static void l4_event_handler(struct net_mgmt_event_callback *cb,
			     uint64_t event,
			     struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	switch (event) {
	case NET_EVENT_L4_CONNECTED:
		LOG_INF("Network connectivity established");
		network_status_notify(NETWORK_CONNECTED);
		break;
	case NET_EVENT_L4_DISCONNECTED:
		LOG_INF("Network connectivity lost");
		network_status_notify(NETWORK_DISCONNECTED);
		break;
	default:
		break;
	}
}

static void connectivity_event_handler(struct net_mgmt_event_callback *cb,
				       uint64_t event,
				       struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (event == NET_EVENT_CONN_IF_FATAL_ERROR) {
		LOG_ERR("NET_EVENT_CONN_IF_FATAL_ERROR");
		/* Notify the network module thread; it can take action there
		 * (the system workqueue / module thread are the right place
		 * to call SEND_FATAL_ERROR if we want to escalate).
		 */
		network_status_notify(NETWORK_DISCONNECTED);
	}
}

static void state_running_entry(void *obj)
{
	ARG_UNUSED(obj);
	LOG_DBG("%s", __func__);

	net_mgmt_init_event_callback(&l4_cb, l4_event_handler, L4_EVENT_MASK);
	net_mgmt_add_event_callback(&l4_cb);

	net_mgmt_init_event_callback(&conn_cb, connectivity_event_handler, CONN_LAYER_EVENT_MASK);
	net_mgmt_add_event_callback(&conn_cb);
}

static enum smf_state_result state_running_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	if (&network_chan == state_object->chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		switch (msg->type) {
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
	LOG_DBG("%s", __func__);
}

static enum smf_state_result state_disconnected_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	if (&network_chan == state_object->chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		switch (msg->type) {
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
	LOG_DBG("%s", __func__);

	/* Bring all conn_mgr-managed interfaces to admin-up. For drivers that
	 * use PM auto-runtime (e.g. modem_cellular) this triggers the modem
	 * to power up and run the dial script. Best-effort connect is also
	 * issued for ifaces that have a connectivity layer registered.
	 */
	err = conn_mgr_all_if_up(true);
	if (err && err != -EALREADY) {
		LOG_ERR("conn_mgr_all_if_up, error: %d", err);
	}

	err = conn_mgr_all_if_connect(true);
	if (err && err != -ENOTSUP) {
		LOG_ERR("conn_mgr_all_if_connect, error: %d", err);
	}
}

static enum smf_state_result state_disconnected_searching_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	if (&network_chan == state_object->chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		switch (msg->type) {
		case NETWORK_CONNECT:
			return SMF_EVENT_HANDLED;
		case NETWORK_SEARCH_STOP: __fallthrough;
		case NETWORK_DISCONNECT:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTING]);
			return SMF_EVENT_HANDLED;
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
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		switch (msg->type) {
		case NETWORK_DISCONNECT:
			return SMF_EVENT_HANDLED;
		case NETWORK_CONNECT:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_SEARCHING]);
			return SMF_EVENT_HANDLED;
		default:
			/* System mode change requests are not implementable through
			 * conn_mgr alone, ignore silently.
			 */
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_connected_entry(void *obj)
{
	ARG_UNUSED(obj);
	LOG_DBG("%s", __func__);
}

static enum smf_state_result state_connected_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	if (&network_chan == state_object->chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		if (msg->type == NETWORK_DISCONNECT) {
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
	LOG_DBG("%s", __func__);

	err = conn_mgr_all_if_disconnect(true);
	if (err) {
		LOG_ERR("conn_mgr_all_if_disconnect, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static enum smf_state_result state_disconnecting_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	if (&network_chan == state_object->chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		if (msg->type == NETWORK_DISCONNECTED) {
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

#if defined(CONFIG_APP_INSPECT_SHELL)
	network_state_ctx = &network_state;
#endif /* CONFIG_APP_INSPECT_SHELL */

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
