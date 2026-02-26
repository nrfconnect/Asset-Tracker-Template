/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <modem/nrf_modem_lib.h>
#include <modem/ntn.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <date_time.h>
#include <zephyr/smf.h>

#include "modem/lte_lc.h"
#include "modem/modem_info.h"
#include "app_common.h"
#include "network_ntn.h"

LOG_MODULE_REGISTER(network, CONFIG_APP_NETWORK_LOG_LEVEL);

BUILD_ASSERT(CONFIG_APP_NETWORK_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_NETWORK_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

/* Public channel */
ZBUS_CHAN_DEFINE(NETWORK_CHAN,
		 struct network_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Private channel for internal state machine events */
enum priv_network_ntn_msg {
	LEO_SATELLITE_PASS_UPCOMING,
	PERIODIC_TN_SEARCH,
	NTN_LEO_SATELLITE_PASS_UPCOMING,
	NTN_SEARCH_GEO_START,
	NTN_WAIT_FOR_SATELLITE_PASS,
	NTN_LOCATION_NEEDED,
};

ZBUS_CHAN_DEFINE(PRIV_NETWORK_NTN_CHAN,
		 enum priv_network_ntn_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Subscriber and channel observation via CHANNEL_LIST X-macro */
ZBUS_MSG_SUBSCRIBER_DEFINE(network);

#define CHANNEL_LIST(X)								\
	X(NETWORK_CHAN,			struct network_msg)			\
	X(PRIV_NETWORK_NTN_CHAN,	enum priv_network_ntn_msg)

#define ADD_OBSERVERS(_chan, _type)	ZBUS_CHAN_ADD_OBS(_chan, network, 0);

CHANNEL_LIST(ADD_OBSERVERS)

#define MAX_MSG_SIZE			MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

/* Work items for timers */
static struct k_work_delayable leo_satellite_search_timer_work;
static struct k_work_delayable periodic_tn_search_timer_work;

/* State enum -- matches network_ntn.puml.
 * Indentation reflects the parent-child hierarchy.
 */
enum network_module_state {
	STATE_RUNNING,
		STATE_DISCONNECTED,
			STATE_DISCONNECTED_IDLE,
			STATE_DISCONNECTED_SEARCHING_TN,
			STATE_DISCONNECTED_WAITING_FOR_LEO,
			STATE_DISCONNECTED_NTN_SEARCH,
				STATE_NTN_SEARCH_PREPARE,
				STATE_NTN_SEARCH_LEO,
				STATE_NTN_SEARCH_GEO,
		STATE_CONNECTED,
		STATE_DISCONNECTING,
};

struct network_state_object {
	/* This must be first */
	struct smf_ctx ctx;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;

	/* Buffer for last ZBus message */
	uint8_t msg_buf[MAX_MSG_SIZE];

	/* Cached location for NTN. Should always contain the last known location. */
	struct {
		double latitude;
		double longitude;
		float altitude;
		bool valid;
		int64_t unix_time_ms;
	} location;

	/* Unix time (ms) when TLE data was last stored.
	 * Must survive reboots if TLE is persisted to flash.
	 */
	int64_t tle_unix_time_ms;

	/* Predicted next LEO pass time (Unix time ms) */
	int64_t next_pass_unix_time_ms;

	/* Set when leo_satellite_search_timer is running */
	bool waiting_for_leo;
};

/* Forward declarations of state handlers */
static void state_running_entry(void *obj);
static enum smf_state_result state_running_run(void *obj);

static void state_disconnected_entry(void *obj);
static enum smf_state_result state_disconnected_run(void *obj);

static enum smf_state_result state_disconnected_idle_run(void *obj);

static void state_disconnected_searching_tn_entry(void *obj);
static enum smf_state_result state_disconnected_searching_tn_run(void *obj);

static void state_disconnected_waiting_for_leo_entry(void *obj);
static enum smf_state_result state_disconnected_waiting_for_leo_run(void *obj);
static void state_disconnected_waiting_for_leo_exit(void *obj);

static void state_disconnected_ntn_search_entry(void *obj);
static enum smf_state_result state_disconnected_ntn_search_run(void *obj);

static enum smf_state_result state_ntn_search_prepare_run(void *obj);

static void state_ntn_search_leo_entry(void *obj);

static void state_ntn_search_geo_entry(void *obj);

static void state_connected_entry(void *obj);
static enum smf_state_result state_connected_run(void *obj);

static void state_disconnecting_entry(void *obj);
static enum smf_state_result state_disconnecting_run(void *obj);

/* State table */
static const struct smf_state states[] = {
	[STATE_RUNNING] =
		SMF_CREATE_STATE(state_running_entry, state_running_run, NULL,
				 NULL,
				 &states[STATE_DISCONNECTED]),
	[STATE_DISCONNECTED] =
		SMF_CREATE_STATE(state_disconnected_entry,
				 state_disconnected_run, NULL,
				 &states[STATE_RUNNING],
				 &states[STATE_DISCONNECTED_IDLE]),
	[STATE_DISCONNECTED_IDLE] =
		SMF_CREATE_STATE(NULL, state_disconnected_idle_run, NULL,
				 &states[STATE_DISCONNECTED],
				 NULL),
	[STATE_DISCONNECTED_SEARCHING_TN] =
		SMF_CREATE_STATE(state_disconnected_searching_tn_entry,
				 state_disconnected_searching_tn_run, NULL,
				 &states[STATE_DISCONNECTED],
				 NULL),
	[STATE_DISCONNECTED_WAITING_FOR_LEO] =
		SMF_CREATE_STATE(state_disconnected_waiting_for_leo_entry,
				 state_disconnected_waiting_for_leo_run,
				 state_disconnected_waiting_for_leo_exit,
				 &states[STATE_DISCONNECTED],
				 NULL),
	[STATE_DISCONNECTED_NTN_SEARCH] =
		SMF_CREATE_STATE(state_disconnected_ntn_search_entry,
				 state_disconnected_ntn_search_run, NULL,
				 &states[STATE_DISCONNECTED],
				 &states[STATE_NTN_SEARCH_PREPARE]),
	[STATE_NTN_SEARCH_PREPARE] =
		SMF_CREATE_STATE(NULL, state_ntn_search_prepare_run, NULL,
				 &states[STATE_DISCONNECTED_NTN_SEARCH],
				 NULL),
	[STATE_NTN_SEARCH_LEO] =
		SMF_CREATE_STATE(state_ntn_search_leo_entry, NULL, NULL,
				 &states[STATE_DISCONNECTED_NTN_SEARCH],
				 NULL),
	[STATE_NTN_SEARCH_GEO] =
		SMF_CREATE_STATE(state_ntn_search_geo_entry, NULL, NULL,
				 &states[STATE_DISCONNECTED_NTN_SEARCH],
				 NULL),
	[STATE_CONNECTED] =
		SMF_CREATE_STATE(state_connected_entry, state_connected_run,
				 NULL,
				 &states[STATE_RUNNING],
				 NULL),
	[STATE_DISCONNECTING] =
		SMF_CREATE_STATE(state_disconnecting_entry,
				 state_disconnecting_run, NULL,
				 &states[STATE_RUNNING],
				 NULL),
};

/* Helper: publish a simple network_msg_type on NETWORK_CHAN */
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

static void network_msg_send(const struct network_msg *msg)
{
	int err;

	err = zbus_chan_pub(&NETWORK_CHAN, msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void priv_ntn_msg_send(enum priv_network_ntn_msg msg)
{
	int err;

	err = zbus_chan_pub(&PRIV_NETWORK_NTN_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub (priv), error: %d", err);
		SEND_FATAL_ERROR();
	}
}

/* Delayable work handlers */
static void leo_satellite_search_timer_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	priv_ntn_msg_send(LEO_SATELLITE_PASS_UPCOMING);
}

static void periodic_tn_search_timer_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	priv_ntn_msg_send(PERIODIC_TN_SEARCH);
}

/* LTE Link Controller event handler, runs on lte_lc workqueue */
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
	case LTE_LC_EVT_PDN:
		switch (evt->pdn.type) {
		case LTE_LC_EVT_PDN_ACTIVATED:
			LOG_DBG("PDN connection activated");
			network_status_notify(NETWORK_CONNECTED);

			break;
		case LTE_LC_EVT_PDN_DEACTIVATED:
			LOG_DBG("PDN connection deactivated");
			network_status_notify(NETWORK_DISCONNECTED);

			break;
		case LTE_LC_EVT_PDN_NETWORK_DETACH:
			LOG_DBG("PDN connection network detached");
			network_status_notify(NETWORK_DISCONNECTED);

			break;
		case LTE_LC_EVT_PDN_SUSPENDED:
			LOG_DBG("PDN connection suspended");
			network_status_notify(NETWORK_DISCONNECTED);

			break;
		case LTE_LC_EVT_PDN_RESUMED:
			LOG_DBG("PDN connection resumed");
			network_status_notify(NETWORK_CONNECTED);

			break;
		default:
			break;
		}

		break;
	case LTE_LC_EVT_MODEM_EVENT:
		if (evt->modem_evt.type == LTE_LC_MODEM_EVT_RESET_LOOP) {
			LOG_WRN("The modem has detected a reset loop!");
			network_status_notify(NETWORK_MODEM_RESET_LOOP);
		} else if (evt->modem_evt.type == LTE_LC_MODEM_EVT_LIGHT_SEARCH_DONE) {
			LOG_DBG("LTE_LC_MODEM_EVT_LIGHT_SEARCH_DONE");
			network_status_notify(NETWORK_LIGHT_SEARCH_DONE);
		} else if (evt->modem_evt.type == LTE_LC_MODEM_EVT_SEARCH_DONE) {
			LOG_DBG("LTE_LC_MODEM_EVT_SEARCH_DONE");
			network_status_notify(NETWORK_SEARCH_DONE);
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

		LOG_DBG("eDRX parameters received, mode: %d, eDRX: %0.2f s, PTW: %.02f s",
			msg.edrx_cfg.mode, (double)msg.edrx_cfg.edrx, (double)msg.edrx_cfg.ptw);

		network_msg_send(&msg);

		break;
	}
	default:
		break;
	}
}

/* NTN library event handler (runs on system workqueue) */
static void ntn_evt_handler(const struct ntn_evt *evt)
{
	if (evt->type == NTN_EVT_LOCATION_REQUEST && evt->location_request.requested) {
		priv_ntn_msg_send(NTN_LOCATION_NEEDED);
	}
}

static void request_system_mode(void)
{
	int err;
	struct network_msg msg = {
		.type = NETWORK_SYSTEM_MODE_RESPONSE,
	};
	enum lte_lc_system_mode_preference dummy_preference;

	err = lte_lc_system_mode_get(&msg.system_mode, &dummy_preference);
	if (err) {
		LOG_ERR("lte_lc_system_mode_get, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	network_msg_send(&msg);
}

static int network_disconnect(void)
{
	int err;

	err = lte_lc_offline();
	if (err) {
		LOG_ERR("lte_lc_offline, error: %d", err);

		return err;
	}

	return 0;
}

/* State handlers */

static void state_running_entry(void *obj)
{
	int err;

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("Failed to initialize modem library, error: %d", err);

		return;
	}

	lte_lc_register_handler(lte_lc_evt_handler);
	ntn_register_handler(ntn_evt_handler);

	err = lte_lc_pdn_default_ctx_events_enable();
	if (err) {
		LOG_ERR("lte_lc_pdn_default_ctx_events_enable, error: %d", err);

		return;
	}

	k_work_init_delayable(&leo_satellite_search_timer_work, leo_satellite_search_timer_work_fn);
	k_work_init_delayable(&periodic_tn_search_timer_work, periodic_tn_search_timer_work_fn);

	LOG_DBG("Network NTN module started");
}

static enum smf_state_result state_running_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	if (state_object->chan == &NETWORK_CHAN) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_DISCONNECTED:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED]);

			return SMF_EVENT_HANDLED;
		case NETWORK_UICC_FAILURE:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);

			return SMF_EVENT_HANDLED;
		case NETWORK_SYSTEM_MODE_REQUEST:
			request_system_mode();

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
	/* TODO: implement in state-handlers task */

	ARG_UNUSED(obj);

	return SMF_EVENT_PROPAGATE;
}

static enum smf_state_result state_disconnected_idle_run(void *obj)
{
	/* TODO: implement in state-handlers task */

	ARG_UNUSED(obj);

	return SMF_EVENT_PROPAGATE;
}

static void state_disconnected_searching_tn_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);
}

static enum smf_state_result state_disconnected_searching_tn_run(void *obj)
{
	/* TODO: implement in state-handlers task */

	ARG_UNUSED(obj);

	return SMF_EVENT_PROPAGATE;
}

static void state_disconnected_waiting_for_leo_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);
}

static enum smf_state_result state_disconnected_waiting_for_leo_run(void *obj)
{
	/* TODO: implement in state-handlers task */

	ARG_UNUSED(obj);

	return SMF_EVENT_PROPAGATE;
}

static void state_disconnected_waiting_for_leo_exit(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);
}

static void state_disconnected_ntn_search_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);
}

static enum smf_state_result state_disconnected_ntn_search_run(void *obj)
{
	/* TODO: implement in state-handlers task */

	ARG_UNUSED(obj);

	return SMF_EVENT_PROPAGATE;
}

static enum smf_state_result state_ntn_search_prepare_run(void *obj)
{
	/* TODO: implement in state-handlers task */

	ARG_UNUSED(obj);

	return SMF_EVENT_PROPAGATE;
}

static void state_ntn_search_leo_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);
}

static void state_ntn_search_geo_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);
}

static void state_connected_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);
}

static enum smf_state_result state_connected_run(void *obj)
{
	/* TODO: implement in state-handlers task */

	ARG_UNUSED(obj);

	return SMF_EVENT_PROPAGATE;
}

static void state_disconnecting_entry(void *obj)
{
	int err;

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	err = network_disconnect();
	if (err) {
		LOG_ERR("network_disconnect, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static enum smf_state_result state_disconnecting_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	if (state_object->chan == &NETWORK_CHAN) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		if (msg.type == NETWORK_DISCONNECTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* Module thread */

static void network_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Network watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void network_ntn_module_thread(void)
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

K_THREAD_DEFINE(network_ntn_module_thread_id,
		CONFIG_APP_NETWORK_THREAD_STACK_SIZE,
		network_ntn_module_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
