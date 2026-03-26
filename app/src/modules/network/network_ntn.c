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
#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
#include <modem/ntn.h>

#include "app_common.h"
#include "network.h"

LOG_MODULE_REGISTER(network_ntn, CONFIG_APP_NETWORK_NTN_LOG_LEVEL);

BUILD_ASSERT(CONFIG_APP_NETWORK_NTN_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_NETWORK_NTN_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than max message processing time");

/* Resolve the TN system mode from Kconfig. */
#if defined(CONFIG_LTE_NETWORK_MODE_LTE_M_NBIOT_GPS)
#define TN_SYSTEM_MODE		LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS
#elif defined(CONFIG_LTE_NETWORK_MODE_LTE_M_NBIOT)
#define TN_SYSTEM_MODE		LTE_LC_SYSTEM_MODE_LTEM_NBIOT
#elif defined(CONFIG_LTE_NETWORK_MODE_LTE_M_GPS)
#define TN_SYSTEM_MODE		LTE_LC_SYSTEM_MODE_LTEM_GPS
#elif defined(CONFIG_LTE_NETWORK_MODE_LTE_M)
#define TN_SYSTEM_MODE		LTE_LC_SYSTEM_MODE_LTEM
#elif defined(CONFIG_LTE_NETWORK_MODE_NBIOT_GPS)
#define TN_SYSTEM_MODE		LTE_LC_SYSTEM_MODE_NBIOT_GPS
#elif defined(CONFIG_LTE_NETWORK_MODE_NBIOT)
#define TN_SYSTEM_MODE		LTE_LC_SYSTEM_MODE_NBIOT
#else
#define TN_SYSTEM_MODE		LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS
#endif

#define NTN_LOCATION_VALIDITY_S		3600

/* Cached NTN location received via NETWORK_CONNECT_NTN.
 * Re-sent to the modem when the NTN library requests a location update.
 */
static struct {
	double latitude;
	double longitude;
	float altitude;
	bool valid;
} ntn_cached_location;

/* Channels and subscribers */

ZBUS_CHAN_DEFINE(network_chan,
		struct network_msg,
		NULL, NULL,
		ZBUS_OBSERVERS_EMPTY,
		ZBUS_MSG_INIT(0));

ZBUS_MSG_SUBSCRIBER_DEFINE(network_ntn);

ZBUS_CHAN_ADD_OBS(network_chan, network_ntn, 0);

/*
 * Private channel for events that originate outside the module thread
 * (LTE LC callback, NTN library callback, delayed work).
 */
enum priv_ntn_msg_type {
	NTN_PRIV_PDN_ACTIVATED,
	NTN_PRIV_PDN_DEACTIVATED,
	NTN_PRIV_LOCATION_REQUESTED,
	NTN_PRIV_LOCATION_NOT_REQUESTED,
	NTN_PRIV_TN_SEARCH_TIMEOUT,
	NTN_PRIV_LIGHT_SEARCH_DONE,
	NTN_PRIV_SEARCH_DONE,
};

ZBUS_CHAN_DEFINE(priv_ntn_chan,
		enum priv_ntn_msg_type,
		NULL, NULL,
		ZBUS_OBSERVERS(network_ntn),
		ZBUS_MSG_INIT(0));

#define CHANNEL_LIST(X)						\
	X(network_chan,		struct network_msg)		\
	X(priv_ntn_chan,	enum priv_ntn_msg_type)

#define MAX_MSG_SIZE	MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

/* State machine */

enum network_ntn_state {
	STATE_RUNNING,
		STATE_DISCONNECTED,
			STATE_DISCONNECTED_IDLE,
			STATE_TN_SEARCHING,
			STATE_NTN_SEARCHING,
		STATE_CONNECTED_TN,
		STATE_CONNECTED_NTN,
		STATE_DISCONNECTING_TN,
		STATE_DISCONNECTING_NTN,
};

struct ntn_state_object {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	uint8_t msg_buf[MAX_MSG_SIZE];
};

/* Forward declarations */
static void state_running_entry(void *obj);
static enum smf_state_result state_running_run(void *obj);
static void state_disconnected_entry(void *obj);
static enum smf_state_result state_disconnected_run(void *obj);
static enum smf_state_result state_disconnected_idle_run(void *obj);
static void state_tn_searching_entry(void *obj);
static enum smf_state_result state_tn_searching_run(void *obj);
static void state_tn_searching_exit(void *obj);
static void state_ntn_searching_entry(void *obj);
static enum smf_state_result state_ntn_searching_run(void *obj);
static void state_connected_tn_entry(void *obj);
static enum smf_state_result state_connected_tn_run(void *obj);
static void state_connected_ntn_entry(void *obj);
static enum smf_state_result state_connected_ntn_run(void *obj);
static void state_disconnecting_tn_entry(void *obj);
static enum smf_state_result state_disconnecting_tn_run(void *obj);
static void state_disconnecting_ntn_entry(void *obj);
static enum smf_state_result state_disconnecting_ntn_run(void *obj);

static const struct smf_state states[] = {
	[STATE_RUNNING] = SMF_CREATE_STATE(
		state_running_entry, state_running_run, NULL,
		NULL, &states[STATE_DISCONNECTED]),
#if defined(CONFIG_APP_NETWORK_NTN_SEARCH_NETWORK_ON_STARTUP)
	[STATE_DISCONNECTED] = SMF_CREATE_STATE(
		state_disconnected_entry, state_disconnected_run, NULL,
		&states[STATE_RUNNING],
		&states[STATE_TN_SEARCHING]),
#else
	[STATE_DISCONNECTED] = SMF_CREATE_STATE(
		state_disconnected_entry, state_disconnected_run, NULL,
		&states[STATE_RUNNING],
		&states[STATE_DISCONNECTED_IDLE]),
#endif
	[STATE_DISCONNECTED_IDLE] = SMF_CREATE_STATE(
		NULL, state_disconnected_idle_run, NULL,
		&states[STATE_DISCONNECTED], NULL),
	[STATE_TN_SEARCHING] = SMF_CREATE_STATE(
		state_tn_searching_entry, state_tn_searching_run,
		state_tn_searching_exit,
		&states[STATE_DISCONNECTED], NULL),
	[STATE_NTN_SEARCHING] = SMF_CREATE_STATE(
		state_ntn_searching_entry, state_ntn_searching_run,
		NULL,
		&states[STATE_DISCONNECTED], NULL),
	[STATE_CONNECTED_TN] = SMF_CREATE_STATE(
		state_connected_tn_entry, state_connected_tn_run, NULL,
		&states[STATE_RUNNING], NULL),
	[STATE_CONNECTED_NTN] = SMF_CREATE_STATE(
		state_connected_ntn_entry, state_connected_ntn_run,
		NULL,
		&states[STATE_RUNNING], NULL),
	[STATE_DISCONNECTING_TN] = SMF_CREATE_STATE(
		state_disconnecting_tn_entry,
		state_disconnecting_tn_run, NULL,
		&states[STATE_RUNNING], NULL),
	[STATE_DISCONNECTING_NTN] = SMF_CREATE_STATE(
		state_disconnecting_ntn_entry,
		state_disconnecting_ntn_run, NULL,
		&states[STATE_RUNNING], NULL),
};

/* Timeout delayed work */

static void tn_search_timeout_work_fn(struct k_work *work)
{
	int err;
	enum priv_ntn_msg_type msg = NTN_PRIV_TN_SEARCH_TIMEOUT;

	ARG_UNUSED(work);

	err = zbus_chan_pub(&priv_ntn_chan, &msg, K_NO_WAIT);
	if (err) {
		LOG_ERR("Failed to publish TN search timeout: %d", err);
	}
}

static K_WORK_DELAYABLE_DEFINE(tn_search_timeout_work, tn_search_timeout_work_fn);

/* NTN library event handler (runs in NTN workqueue context) */

static void ntn_evt_handler(const struct ntn_evt *evt)
{
	int err;
	enum priv_ntn_msg_type msg;

	switch (evt->type) {
	case NTN_EVT_LOCATION_REQUEST:
		if (evt->location_request.requested) {
			LOG_INF("NTN location requested, acc: %d m",
				evt->location_request.accuracy);
			msg = NTN_PRIV_LOCATION_REQUESTED;
		} else {
			LOG_INF("NTN location no longer requested");
			msg = NTN_PRIV_LOCATION_NOT_REQUESTED;
		}

		err = zbus_chan_pub(&priv_ntn_chan, &msg, K_NO_WAIT);
		if (err) {
			LOG_ERR("priv_ntn_chan pub: %d", err);
		}
		break;
	default:
		LOG_WRN("Unknown NTN event: %d", evt->type);
		break;
	}
}

/* Helper functions */

static void priv_notify(enum priv_ntn_msg_type type)
{
	int err;

	err = zbus_chan_pub(&priv_ntn_chan, &type, K_NO_WAIT);
	if (err) {
		LOG_ERR("priv_ntn_chan pub: %d", err);
	}
}

static void network_status_notify(enum network_msg_type type)
{
	int err;
	struct network_msg msg = { .type = type };

	err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("network_chan pub: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void network_connected_notify(enum network_connection_type conn_type)
{
	int err;
	struct network_msg msg = {
		.type = NETWORK_CONNECTED,
		.connection_type = conn_type,
	};

	err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("network_chan pub: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void network_msg_send(const struct network_msg *msg)
{
	int err;

	err = zbus_chan_pub(&network_chan, msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("network_chan pub: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void ntn_location_cache_and_set(double lat, double lon, float alt)
{
	int err;

	ntn_cached_location.latitude = lat;
	ntn_cached_location.longitude = lon;
	ntn_cached_location.altitude = alt;
	ntn_cached_location.valid = true;

	err = ntn_location_set(lat, lon, alt, NTN_LOCATION_VALIDITY_S);
	if (err) {
		LOG_ERR("ntn_location_set: %d", err);
	}
}

static void request_system_mode(void)
{
	int err;
	struct network_msg msg = {
		.type = NETWORK_SYSTEM_MODE_RESPONSE,
	};
	enum lte_lc_system_mode_preference pref;

	err = lte_lc_system_mode_get(&msg.system_mode, &pref);
	if (err) {
		LOG_ERR("lte_lc_system_mode_get: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	network_msg_send(&msg);
}

/* LTE LC event handler (runs in modem library context)
 *
 * PDN events are forwarded through the private channel so the
 * state machine can enrich NETWORK_CONNECTED with connection type.
 * All other events are published directly on network_chan.
 */

static void lte_lc_evt_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if (evt->nw_reg_status == LTE_LC_NW_REG_UICC_FAIL) {
			LOG_ERR("No SIM card detected!");
			network_status_notify(NETWORK_UICC_FAILURE);
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_NOT_REGISTERED) {
			LOG_WRN("Not registered");
			network_status_notify(NETWORK_ATTACH_REJECTED);
		}

		break;

	case LTE_LC_EVT_PDN:
		switch (evt->pdn.type) {
		case LTE_LC_EVT_PDN_ACTIVATED:
			LOG_DBG("PDN activated");
			priv_notify(NTN_PRIV_PDN_ACTIVATED);
			break;
		case LTE_LC_EVT_PDN_RESUMED:
			LOG_DBG("PDN resumed");
			priv_notify(NTN_PRIV_PDN_ACTIVATED);
			break;
		case LTE_LC_EVT_PDN_DEACTIVATED:
			LOG_DBG("PDN deactivated");
			priv_notify(NTN_PRIV_PDN_DEACTIVATED);
			break;
		case LTE_LC_EVT_PDN_NETWORK_DETACH:
			LOG_DBG("PDN network detach");
			priv_notify(NTN_PRIV_PDN_DEACTIVATED);
			break;
		case LTE_LC_EVT_PDN_SUSPENDED:
			LOG_DBG("PDN suspended");
			priv_notify(NTN_PRIV_PDN_DEACTIVATED);
			break;
		default:
			break;
		}

		break;

	case LTE_LC_EVT_MODEM_EVENT:
		if (evt->modem_evt.type == LTE_LC_MODEM_EVT_RESET_LOOP) {
			LOG_WRN("Modem reset loop detected!");
			network_status_notify(NETWORK_MODEM_RESET_LOOP);
		} else if (evt->modem_evt.type == LTE_LC_MODEM_EVT_LIGHT_SEARCH_DONE) {
			LOG_DBG("Light search done");
			priv_notify(NTN_PRIV_LIGHT_SEARCH_DONE);
		} else if (evt->modem_evt.type == LTE_LC_MODEM_EVT_SEARCH_DONE) {
			LOG_DBG("Search done");
			priv_notify(NTN_PRIV_SEARCH_DONE);
		}

		break;

	case LTE_LC_EVT_PSM_UPDATE: {
		struct network_msg msg = {
			.type = NETWORK_PSM_PARAMS,
			.psm_cfg = evt->psm_cfg,
		};

		LOG_DBG("PSM TAU: %d, Active: %d", msg.psm_cfg.tau, msg.psm_cfg.active_time);
		network_msg_send(&msg);

		break;
	}

	case LTE_LC_EVT_EDRX_UPDATE: {
		struct network_msg msg = {
			.type = NETWORK_EDRX_PARAMS,
			.edrx_cfg = evt->edrx_cfg,
		};

		LOG_DBG("eDRX mode: %d, val: %.2f s, PTW: %.2f s",
			msg.edrx_cfg.mode,
			(double)msg.edrx_cfg.edrx,
			(double)msg.edrx_cfg.ptw);
		network_msg_send(&msg);

		break;
	}

	default:
		break;
	}
}

/* ----------------------------------------------------------------
 * State handlers
 * ---------------------------------------------------------------- */

static void state_running_entry(void *o)
{
	int err;

	ARG_UNUSED(o);
	LOG_DBG("state_running_entry");

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("nrf_modem_lib_init: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	lte_lc_register_handler(lte_lc_evt_handler);

	err = lte_lc_pdn_default_ctx_events_enable();
	if (err) {
		LOG_ERR("pdn_default_ctx_events_enable: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	ntn_register_handler(ntn_evt_handler);

	LOG_INF("NTN network module started");
}

static enum smf_state_result state_running_run(void *obj)
{
	struct ntn_state_object const *state_object = obj;

	if (&priv_ntn_chan == state_object->chan) {
		enum priv_ntn_msg_type priv_msg =
			*(const enum priv_ntn_msg_type *)state_object->msg_buf;

		if (priv_msg == NTN_PRIV_PDN_DEACTIVATED) {
			network_status_notify(NETWORK_DISCONNECTED);
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED]);

			return SMF_EVENT_HANDLED;
		}
	}

	if (&network_chan == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
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

/* STATE_DISCONNECTED (parent for all disconnected sub-states) */

static void state_disconnected_entry(void *obj)
{
	ARG_UNUSED(obj);
	LOG_DBG("state_disconnected_entry");
}

static enum smf_state_result state_disconnected_run(void *obj)
{
	struct ntn_state_object const *state_object = obj;

	if (&priv_ntn_chan == state_object->chan) {
		enum priv_ntn_msg_type priv_msg =
			*(const enum priv_ntn_msg_type *)state_object->msg_buf;

		if (priv_msg == NTN_PRIV_PDN_DEACTIVATED) {
			return SMF_EVENT_HANDLED;
		}
	}

	if (&network_chan == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		if (msg.type == NETWORK_DISCONNECTED || msg.type == NETWORK_CONNECTED) {
			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* STATE_DISCONNECTED_IDLE */

static enum smf_state_result state_disconnected_idle_run(void *obj)
{
	int err;
	struct ntn_state_object const *state_object = obj;

	if (&priv_ntn_chan == state_object->chan) {
		enum priv_ntn_msg_type priv_msg =
			*(const enum priv_ntn_msg_type *)state_object->msg_buf;

		switch (priv_msg) {
		case NTN_PRIV_PDN_ACTIVATED:
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_TN]);

			return SMF_EVENT_HANDLED;
		case NTN_PRIV_LIGHT_SEARCH_DONE:
		case NTN_PRIV_SEARCH_DONE:
			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	if (&network_chan == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_DISCONNECT:
			return SMF_EVENT_HANDLED;
		case NETWORK_CONNECT:
			smf_set_state(SMF_CTX(state_object), &states[STATE_TN_SEARCHING]);

			return SMF_EVENT_HANDLED;
		case NETWORK_CONNECT_NTN:
			LOG_INF("NTN connect: lat=%.6f lon=%.6f alt=%.1f",
				msg.prev_location.latitude,
				msg.prev_location.longitude,
				(double)msg.prev_location.altitude);
			ntn_location_cache_and_set(
				msg.prev_location.latitude,
				msg.prev_location.longitude,
				msg.prev_location.altitude);
			smf_set_state(SMF_CTX(state_object), &states[STATE_NTN_SEARCHING]);

			return SMF_EVENT_HANDLED;
		case NETWORK_SYSTEM_MODE_SET_LTEM:
			err = lte_lc_system_mode_set(
				LTE_LC_SYSTEM_MODE_LTEM,
				LTE_LC_SYSTEM_MODE_PREFER_AUTO);
			if (err) {
				LOG_ERR("sys_mode LTEM: %d", err);
				SEND_FATAL_ERROR();
			}

			return SMF_EVENT_HANDLED;
		case NETWORK_SYSTEM_MODE_SET_NBIOT:
			err = lte_lc_system_mode_set(
				LTE_LC_SYSTEM_MODE_NBIOT,
				LTE_LC_SYSTEM_MODE_PREFER_AUTO);
			if (err) {
				LOG_ERR("sys_mode NBIOT: %d", err);
				SEND_FATAL_ERROR();
			}

			return SMF_EVENT_HANDLED;
		case NETWORK_SYSTEM_MODE_SET_LTEM_NBIOT:
			err = lte_lc_system_mode_set(
				LTE_LC_SYSTEM_MODE_LTEM_NBIOT,
				LTE_LC_SYSTEM_MODE_PREFER_AUTO);
			if (err) {
				LOG_ERR("sys_mode LM+NB: %d", err);
				SEND_FATAL_ERROR();
			}

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* STATE_TN_SEARCHING */

static void state_tn_searching_entry(void *obj)
{
	int err;

	ARG_UNUSED(obj);
	LOG_DBG("state_tn_searching_entry");

	err = lte_lc_system_mode_set(TN_SYSTEM_MODE, LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	if (err) {
		LOG_ERR("sys_mode TN: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	err = lte_lc_connect_async(lte_lc_evt_handler);
	if (err) {
		LOG_ERR("lte_lc_connect_async: %d", err);
		SEND_FATAL_ERROR();
	}

	k_work_schedule(&tn_search_timeout_work,
			K_SECONDS(CONFIG_APP_NETWORK_NTN_TN_SEARCH_TIMEOUT_SECONDS));
}

static enum smf_state_result state_tn_searching_run(void *obj)
{
	int err;
	struct ntn_state_object const *state_object = obj;

	if (&priv_ntn_chan == state_object->chan) {
		enum priv_ntn_msg_type priv_msg =
			*(const enum priv_ntn_msg_type *)state_object->msg_buf;

		switch (priv_msg) {
		case NTN_PRIV_PDN_ACTIVATED:
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_TN]);

			return SMF_EVENT_HANDLED;
		case NTN_PRIV_TN_SEARCH_TIMEOUT:
			LOG_WRN("TN search timeout, waiting for NTN command");
			network_status_notify(NETWORK_SEARCH_DONE);
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);

			return SMF_EVENT_HANDLED;
#if defined(CONFIG_APP_NETWORK_NTN_TN_SEARCH_USE_LIGHT_SEARCH)
		case NTN_PRIV_LIGHT_SEARCH_DONE:
			LOG_INF("TN light search done, waiting for NTN command");
			network_status_notify(NETWORK_LIGHT_SEARCH_DONE);
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);

			return SMF_EVENT_HANDLED;
#else
		case NTN_PRIV_SEARCH_DONE:
			LOG_INF("TN search done, waiting for NTN command");
			network_status_notify(NETWORK_SEARCH_DONE);
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);

			return SMF_EVENT_HANDLED;
#endif
		default:
			break;
		}
	}

	if (&network_chan == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_CONNECT:
			return SMF_EVENT_HANDLED;
		case NETWORK_SEARCH_STOP: __fallthrough;
		case NETWORK_DISCONNECT:
			err = lte_lc_offline();
			if (err) {
				LOG_ERR("lte_lc_offline: %d", err);
				SEND_FATAL_ERROR();
			}

			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_tn_searching_exit(void *obj)
{
	ARG_UNUSED(obj);

	k_work_cancel_delayable(&tn_search_timeout_work);
}

/* STATE_NTN_SEARCHING */

static void state_ntn_searching_entry(void *obj)
{
	int err;

	ARG_UNUSED(obj);
	LOG_DBG("state_ntn_searching_entry");

	err = lte_lc_offline();
	if (err) {
		LOG_ERR("lte_lc_offline: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_NTN_NBIOT, LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	if (err) {
		LOG_ERR("sys_mode NTN: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	err = lte_lc_connect_async(lte_lc_evt_handler);
	if (err) {
		LOG_ERR("lte_lc_connect_async NTN: %d", err);
		SEND_FATAL_ERROR();
	}
}

static enum smf_state_result state_ntn_searching_run(void *obj)
{
	int err;
	struct ntn_state_object const *state_object = obj;

	if (&priv_ntn_chan == state_object->chan) {
		enum priv_ntn_msg_type priv_msg =
			*(const enum priv_ntn_msg_type *)state_object->msg_buf;

		switch (priv_msg) {
		case NTN_PRIV_PDN_ACTIVATED:
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_NTN]);

			return SMF_EVENT_HANDLED;
		case NTN_PRIV_SEARCH_DONE:
			LOG_WRN("NTN search done, no network");

			err = lte_lc_offline();
			if (err) {
				LOG_ERR("lte_lc_offline: %d", err);
			}

			err = lte_lc_system_mode_set(TN_SYSTEM_MODE, LTE_LC_SYSTEM_MODE_PREFER_AUTO);
			if (err) {
				LOG_ERR("sys_mode TN restore: %d", err);
			}

			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	if (&network_chan == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_SEARCH_STOP: __fallthrough;
		case NETWORK_DISCONNECT:
			err = lte_lc_offline();
			if (err) {
				LOG_ERR("lte_lc_offline: %d", err);
				SEND_FATAL_ERROR();
			}

			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* STATE_CONNECTED_TN */

static void state_connected_tn_entry(void *obj)
{
	ARG_UNUSED(obj);
	LOG_INF("Connected via TN");

	network_connected_notify(NETWORK_CONNECTION_TN);
}

static enum smf_state_result state_connected_tn_run(void *obj)
{
	struct ntn_state_object const *state_object = obj;

	if (&network_chan == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_DISCONNECT:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTING_TN]);

			return SMF_EVENT_HANDLED;
		case NETWORK_CONNECTED:
			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	if (&priv_ntn_chan == state_object->chan) {
		enum priv_ntn_msg_type priv_msg =
			*(const enum priv_ntn_msg_type *)state_object->msg_buf;

		if (priv_msg == NTN_PRIV_PDN_ACTIVATED) {
			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* STATE_CONNECTED_NTN */

static void state_connected_ntn_entry(void *obj)
{
	ARG_UNUSED(obj);
	LOG_INF("Connected via NTN");

	network_connected_notify(NETWORK_CONNECTION_NTN);
}

static enum smf_state_result state_connected_ntn_run(void *obj)
{
	struct ntn_state_object const *state_object = obj;

	if (&network_chan == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_DISCONNECT:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTING_NTN]);

			return SMF_EVENT_HANDLED;
		case NETWORK_CONNECTED:
			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	if (&priv_ntn_chan == state_object->chan) {
		enum priv_ntn_msg_type priv_msg =
			*(const enum priv_ntn_msg_type *)state_object->msg_buf;

		switch (priv_msg) {
		case NTN_PRIV_PDN_ACTIVATED:
			return SMF_EVENT_HANDLED;
		case NTN_PRIV_LOCATION_REQUESTED:
			if (ntn_cached_location.valid) {
				LOG_INF("Re-sending cached NTN location");
				ntn_location_cache_and_set(
					ntn_cached_location.latitude,
					ntn_cached_location.longitude,
					ntn_cached_location.altitude);
			} else {
				LOG_WRN("NTN location requested, no cache");
			}

			return SMF_EVENT_HANDLED;
		case NTN_PRIV_LOCATION_NOT_REQUESTED:
			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* STATE_DISCONNECTING_TN */

static void state_disconnecting_tn_entry(void *obj)
{
	int err;

	ARG_UNUSED(obj);
	LOG_DBG("state_disconnecting_tn_entry");

	err = lte_lc_offline();
	if (err) {
		LOG_ERR("lte_lc_offline: %d", err);
		SEND_FATAL_ERROR();
	}
}

static enum smf_state_result state_disconnecting_tn_run(void *obj)
{
	struct ntn_state_object const *state_object = obj;

	if (&priv_ntn_chan == state_object->chan) {
		enum priv_ntn_msg_type priv_msg =
			*(const enum priv_ntn_msg_type *)state_object->msg_buf;

		if (priv_msg == NTN_PRIV_PDN_DEACTIVATED) {
			network_status_notify(NETWORK_DISCONNECTED);
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* STATE_DISCONNECTING_NTN */

static void state_disconnecting_ntn_entry(void *obj)
{
	int err;

	ARG_UNUSED(obj);
	LOG_DBG("state_disconnecting_ntn_entry");

	err = lte_lc_offline();
	if (err) {
		LOG_ERR("lte_lc_offline: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	err = lte_lc_system_mode_set(TN_SYSTEM_MODE, LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	if (err) {
		LOG_ERR("sys_mode TN restore: %d", err);
		SEND_FATAL_ERROR();
	}
}

static enum smf_state_result state_disconnecting_ntn_run(void *obj)
{
	struct ntn_state_object const *state_object = obj;

	if (&priv_ntn_chan == state_object->chan) {
		enum priv_ntn_msg_type priv_msg =
			*(const enum priv_ntn_msg_type *)state_object->msg_buf;

		if (priv_msg == NTN_PRIV_PDN_DEACTIVATED) {
			network_status_notify(NETWORK_DISCONNECTED);
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* Module thread */

static void ntn_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("NTN watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void network_ntn_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms =
		CONFIG_APP_NETWORK_NTN_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC;
	const uint32_t execution_time_ms =
		CONFIG_APP_NETWORK_NTN_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC;
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	static struct ntn_state_object ntn_state;

	task_wdt_id = task_wdt_add(wdt_timeout_ms, ntn_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("task_wdt_add: %d", task_wdt_id);
		SEND_FATAL_ERROR();

		return;
	}

	smf_set_initial(SMF_CTX(&ntn_state), &states[STATE_RUNNING]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed: %d", err);
			SEND_FATAL_ERROR();

			return;
		}

		err = zbus_sub_wait_msg(&network_ntn, &ntn_state.chan,
					ntn_state.msg_buf, zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg: %d", err);
			SEND_FATAL_ERROR();

			return;
		}

		err = smf_run_state(SMF_CTX(&ntn_state));
		if (err) {
			LOG_ERR("smf_run_state: %d", err);
			SEND_FATAL_ERROR();

			return;
		}
	}
}

K_THREAD_DEFINE(network_ntn_thread_id,
		CONFIG_APP_NETWORK_NTN_THREAD_STACK_SIZE,
		network_ntn_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
