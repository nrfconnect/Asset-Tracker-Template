/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <date_time.h>
#include <zephyr/smf.h>

#include "modem/lte_lc.h"
#include "modem/modem_info.h"
#include "modules_common.h"
#include "message_channel.h"

/* Register log module */
LOG_MODULE_REGISTER(network, CONFIG_APP_NETWORK_LOG_LEVEL);

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(network);

/* Observe network channel */
ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, network, 0);

#define MAX_MSG_SIZE sizeof(struct network_msg)

/* Macros used to subscribe to specific Zephyr NET management events. */
#define L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
#define CONN_LAYER_EVENT_MASK (NET_EVENT_CONN_IF_FATAL_ERROR)

/* Zephyr NET management event callback structures. */
static struct net_mgmt_event_callback l4_cb;
static struct net_mgmt_event_callback conn_cb;


/* State machine */

/* Network module states.
 */
enum network_module_state {
	/* The module is running */
	STATE_RUNNING,

	/* The device is not connected to a network */
	STATE_DISCONNECTED,

	/* The device is disconencted from network and is not searching */
	STATE_DISCONNECTED_IDLE,

	/* The device is disconnected and the modem is searching for available networks */
	STATE_DISCONNECTED_SEARCHING,

	/* The device is connected to a network */
	STATE_CONNECTED,

	/* The device has initiated detachment from network, but the modem has not confirmed
	 * detachment yet.
	 */
	STATE_DISCONNECTING,
};

/* State object.
 * Used to transfer context data between state changes.
 */
struct state_object {
	/* This must be first */
	struct smf_ctx ctx;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;

	/* Buffer for last ZBus message */
	uint8_t msg_buf[MAX_MSG_SIZE];
};

/* Forward declarations of state handlers */
static void state_running_entry(void *obj);
static void state_running_run(void *obj);
static void state_disconnected_entry(void *obj);
static void state_disconnected_run(void *obj);
static void state_disconnected_idle_run(void *obj);
static void state_disconnected_searching_entry(void *obj);
static void state_disconnected_searching_run(void *obj);
static void state_disconnecting_entry(void *obj);
static void state_connected_run(void *obj);
static void state_connected_entry(void *obj);

static struct state_object network_state;

/* State machine definition */
static const struct smf_state states[] = {
	[STATE_RUNNING] =
		SMF_CREATE_STATE(state_running_entry, state_running_run, NULL,
				 NULL,	/* No parent state */
				 &states[STATE_DISCONNECTED]),
	[STATE_DISCONNECTED] =
		SMF_CREATE_STATE(state_disconnected_entry, state_disconnected_run, NULL,
				 &states[STATE_RUNNING],
				 &states[STATE_DISCONNECTED_SEARCHING]),
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
		SMF_CREATE_STATE(state_disconnecting_entry, NULL, NULL,
				 &states[STATE_RUNNING],
				 NULL), /* No initial transition */
};

static void network_status_notify(enum network_msg_type status)
{
	int err;
	struct network_msg msg = {
		.type = status,
	};

	err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

static void network_msg_send(struct network_msg *msg)
{
	int err;

	err = zbus_chan_pub(&NETWORK_CHAN, msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void l4_event_handler(struct net_mgmt_event_callback *cb,
			     uint32_t event,
			     struct net_if *iface)
{
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
		/* Don't care */
		return;
	}
}

static void connectivity_event_handler(struct net_mgmt_event_callback *cb,
				       uint32_t event,
				       struct net_if *iface)
{
	if (event == NET_EVENT_CONN_IF_FATAL_ERROR) {
		LOG_ERR("NET_EVENT_CONN_IF_FATAL_ERROR");
		SEND_FATAL_ERROR();
		return;
	}
}

static void lte_lc_evt_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if (evt->nw_reg_status == LTE_LC_NW_REG_UICC_FAIL) {
			LOG_ERR("No SIM card detected!");
			network_status_notify(NETWORK_UICC_FAILURE);
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_NOT_REGISTERED) {
			LOG_WRN("Not registered, check rejection cause");
			network_status_notify(NETWORK_ATTACH_REJECTED);
		}
		break;
	case LTE_LC_EVT_MODEM_EVENT:
		/* If a reset loop happens in the field, it should not be necessary
		 * to perform any action. The modem will try to re-attach to the LTE network after
		 * the 30-minute block.
		 */
		if (evt->modem_evt == LTE_LC_MODEM_EVT_RESET_LOOP) {
			LOG_WRN("The modem has detected a reset loop!");
			network_status_notify(NETWORK_MODEM_RESET_LOOP);
		} else if (evt->modem_evt == LTE_LC_MODEM_EVT_LIGHT_SEARCH_DONE) {
			LOG_DBG("LTE_LC_MODEM_EVT_LIGHT_SEARCH_DONE");
			network_status_notify(NETWORK_LIGHT_SERACH_DONE);
		}
		break;
	case LTE_LC_EVT_PSM_UPDATE: {
		struct network_msg msg = {
			.type = NETWORK_PSM_PARAMS,
			.psm_cfg = evt->psm_cfg,
		};

		LOG_DBG("PSM parameters received, TAU: %d, Active time: %d",
			msg.psm_cfg.tau, msg.psm_cfg.active_time);

		network_msg_send(&msg);

		break;
	}
	case LTE_LC_EVT_EDRX_UPDATE: {
		struct network_msg msg = {
			.type = NETWORK_EDRX_PARAMS,
			.edrx_cfg = evt->edrx_cfg,
		};

		LOG_DBG("eDRX parameters received, mode: %d, eDRX: %0.2f s, PTW: %f s",
			msg.edrx_cfg.mode, (double)msg.edrx_cfg.edrx, (double)msg.edrx_cfg.ptw);

		network_msg_send(&msg);

		break;
	}
	default:
		break;
	}
}

static void sample_network_quality(void)
{
	int ret;
	struct network_msg msg = {
		.type = NETWORK_QUALITY_SAMPLE_RESPONSE,
	};

	ret = lte_lc_conn_eval_params_get(&msg.conn_eval_params);
	if (ret == -EOPNOTSUPP) {
		LOG_WRN("Connection evaluation not supported in current functional mode");
		return;
	} else if (ret < 0) {
		LOG_ERR("lte_lc_conn_eval_params_get, error: %d", ret);
		SEND_FATAL_ERROR();
		return;
	} else if (ret > 0) {
		LOG_WRN("Connection evaluation failed due to a network related reason: %d", ret);
		return;
	}

	network_msg_send(&msg);
}

static int network_disconnect(void)
{
	int err;

	err = conn_mgr_all_if_disconnect(true);
	if (err) {
		LOG_ERR("conn_mgr_all_if_down, error: %d", err);
		SEND_FATAL_ERROR();
		return err;
	}

	return 0;
}


/* State handlers */

static void state_running_entry(void *obj)
{
	int err;

	ARG_UNUSED(obj);

	LOG_DBG("state_running_entry");

	/* Setup handler for Zephyr NET Connection Manager events. */
	net_mgmt_init_event_callback(&l4_cb, l4_event_handler, L4_EVENT_MASK);
	net_mgmt_add_event_callback(&l4_cb);

	/* Setup handler for Zephyr NET Connection Manager Connectivity layer. */
	net_mgmt_init_event_callback(&conn_cb, connectivity_event_handler, CONN_LAYER_EVENT_MASK);
	net_mgmt_add_event_callback(&conn_cb);

	/* Connecting to the configured connectivity layer. */
	LOG_INF("Bringing network interface up and connecting to the network");

	err = conn_mgr_all_if_up(true);
	if (err) {
		LOG_ERR("conn_mgr_all_if_up, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	lte_lc_register_handler(lte_lc_evt_handler);

	LOG_DBG("Network module started");
}

static void state_running_run(void *obj)
{
	struct state_object const *state_object = obj;

	LOG_DBG("state_running_run");

	if (&NETWORK_CHAN == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_DISCONNECTED:
			STATE_SET(network_state, STATE_DISCONNECTED);
			break;
		case NETWORK_UICC_FAILURE:
			STATE_SET(network_state, STATE_DISCONNECTED_IDLE);
			break;
		case NETWORK_QUALITY_SAMPLE_REQUEST:
			sample_network_quality();
			break;
		default:
			break;
		}
	}
}

static void state_disconnected_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("state_disconnected_entry");

	network_status_notify(NETWORK_DISCONNECTED);

	/* Resend connection status if the sample is built for Native Sim.
	 * This is necessary because the network interface is automatically brought up
	 * at SYS_INIT() before main() is called.
	 * This means that NET_EVENT_L4_CONNECTED fires before the
	 * appropriate handler l4_event_handler() is registered.
	 */
	if (IS_ENABLED(CONFIG_BOARD_NATIVE_SIM)) {
		conn_mgr_mon_resend_status();
	}
}

static void state_disconnected_run(void *obj)
{
	struct state_object const *state_object = obj;

	LOG_DBG("state_disconnected_run");

	if (&NETWORK_CHAN == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_CONNECTED:
			STATE_SET(network_state, STATE_CONNECTED);
			break;
		case NETWORK_DISCONNECTED:
			STATE_EVENT_HANDLED(network_state);
			break;
		default:
			break;
		}
	}
}

static void state_disconnected_searching_entry(void *obj)
{
	int err;

	ARG_UNUSED(obj);

	LOG_DBG("state_disconnected_searching_entry");

	err = conn_mgr_all_if_connect(true);
	if (err) {
		LOG_ERR("conn_mgr_all_if_connect, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	/* Resend connection status if the sample is built for Native Sim.
	 * This is necessary because the network interface is automatically brought up
	 * at SYS_INIT() before main() is called.
	 * This means that NET_EVENT_L4_CONNECTED fires before the
	 * appropriate handler l4_event_handler() is registered.
	 */
	if (IS_ENABLED(CONFIG_BOARD_NATIVE_SIM)) {
		conn_mgr_mon_resend_status();
	}
}

static void state_disconnected_searching_run(void *obj)
{
	struct state_object const *state_object = obj;

	LOG_DBG("state_disconnected_searching_run");

	if (&NETWORK_CHAN == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_CONNECT:
			STATE_EVENT_HANDLED(network_state);
			break;
		case NETWORK_SEARCH_STOP: __fallthrough;
		case NETWORK_DISCONNECT:
			STATE_SET(network_state, STATE_DISCONNECTED_IDLE);
			break;
		default:
			break;
		}
	}
}

static void state_disconnected_idle_run(void *obj)
{
	struct state_object const *state_object = obj;

	LOG_DBG("state_disconnected_idle_run");

	if (&NETWORK_CHAN == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_DISCONNECT:
			STATE_EVENT_HANDLED(network_state);
			break;
		case NETWORK_SEARCH_START: __fallthrough;
		case NETWORK_CONNECT:
			STATE_SET(network_state, STATE_DISCONNECTED_SEARCHING);
			break;
		default:
			break;
		}
	}
}

static void state_connected_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("state_connected_entry");
}

static void state_connected_run(void *obj)
{
	struct state_object const *state_object = obj;

	LOG_DBG("state_connected_run");

	if (!IS_ENABLED(CONFIG_APP_NETWORK_SAMPLE_NETWORK_QUALITY)) {
		return;
	}

	if (&NETWORK_CHAN == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_QUALITY_SAMPLE_REQUEST:
			LOG_DBG("Sampling network quality data");
			sample_network_quality();
			break;
		case NETWORK_DISCONNECT:
			STATE_SET(network_state, STATE_DISCONNECTING);
			break;
		default:
			break;
		}
	}
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
		return;
	}
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
	const uint32_t wdt_timeout_ms = (CONFIG_APP_NETWORK_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms = (CONFIG_APP_NETWORK_EXEC_TIME_SECONDS_MAX * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);

	task_wdt_id = task_wdt_add(wdt_timeout_ms, network_wdt_callback, (void *)k_current_get());

	STATE_SET_INITIAL(network_state, STATE_RUNNING);

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

		err = STATE_RUN(network_state);
		if (err) {
			LOG_ERR("handle_message, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}
	}
}

K_THREAD_DEFINE(network_module_thread_id,
		CONFIG_APP_NETWORK_THREAD_STACK_SIZE,
		network_module_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
