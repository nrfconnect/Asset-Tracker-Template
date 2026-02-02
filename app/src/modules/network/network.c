/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <modem/nrf_modem_lib.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <date_time.h>
#include <zephyr/smf.h>
#include <nrf_modem_gnss.h>

#include "modem/lte_lc.h"
#include "modem/modem_info.h"
#include "app_common.h"
#include "network.h"

/* Register log module */
LOG_MODULE_REGISTER(network, CONFIG_APP_NETWORK_LOG_LEVEL);

BUILD_ASSERT(CONFIG_APP_NETWORK_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_NETWORK_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

/* Define channels provided by this module */
ZBUS_CHAN_DEFINE(NETWORK_CHAN,
		 struct network_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.type = NETWORK_DISCONNECTED)
);

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(network);

/* Observe network channel */
ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, network, 0);

#define MAX_MSG_SIZE sizeof(struct network_msg)

/* State machine */

/* Network module states.
 */
enum network_module_state {
	/* The module is running */
	STATE_RUNNING,
		/* The device is not connected to a network */
		STATE_DISCONNECTED,
			/* The device is disconnected from network and is not searching */
			STATE_DISCONNECTED_IDLE,
			/* The device is disconnected and the modem is searching for networks */
			STATE_DISCONNECTED_SEARCHING,
		/* The device is connected to a network */
		STATE_CONNECTED,

		/* The device has initiated detachment from network, but the modem has not confirmed
		 * detachment yet.
		 */
		STATE_DISCONNECTING,
	/* The module is in NTN mode */
	STATE_NTN,
		/* The device connect to NTN network */
		STATE_NTN_CONNECT,
		/* The device is searching for GNSS fix */
		STATE_NTN_GNSS,
		/* The device is in idle mode */
		STATE_NTN_IDLE,
		/* The device is performing TN background scan */
		STATE_NTN_BACK_SCAN,
};

/* State object.
 * Used to transfer context data between state changes.
 */
struct network_state_object {
	/* This must be first */
	struct smf_ctx ctx;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;

	/* Buffer for last ZBus message */
	uint8_t msg_buf[MAX_MSG_SIZE];

	/* GNSS location data */
	struct nrf_modem_gnss_pvt_data_frame last_pvt;

	/* Location validity end time */
	uint64_t location_validity_end_time;
};

/* Forward declarations */
static struct k_work gnss_location_work;
static struct k_work gnss_timeout_work;
static void gnss_event_handler(int event);

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
static void state_ntn_entry(void *obj);
static enum smf_state_result state_ntn_run(void *obj);
static void state_ntn_exit(void *obj);
static void state_ntn_connect_entry(void *obj);
static enum smf_state_result state_ntn_connect_run(void *obj);
static void state_ntn_connect_exit(void *obj);
static void state_ntn_gnss_entry(void *obj);
static enum smf_state_result state_ntn_gnss_run(void *obj);
static void state_ntn_gnss_exit(void *obj);
static void state_ntn_idle_entry(void *obj);
static enum smf_state_result state_ntn_idle_run(void *obj);
static void state_ntn_idle_exit(void *obj);
static void state_ntn_back_scan_entry(void *obj);
static enum smf_state_result state_ntn_back_scan_run(void *obj);
static void state_ntn_back_scan_exit(void *obj);

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
	[STATE_NTN] =
		SMF_CREATE_STATE(state_ntn_entry, state_ntn_run, state_ntn_exit,
				 NULL,	/* No parent state */
				 &states[STATE_NTN_CONNECT]),
	[STATE_NTN_CONNECT] =
		SMF_CREATE_STATE(state_ntn_connect_entry, state_ntn_connect_run, state_ntn_connect_exit,
				 &states[STATE_NTN],
				 NULL),
	[STATE_NTN_GNSS] =
		SMF_CREATE_STATE(state_ntn_gnss_entry, state_ntn_gnss_run, state_ntn_gnss_exit,
				 &states[STATE_NTN],
				 NULL),
	[STATE_NTN_IDLE] =
		SMF_CREATE_STATE(state_ntn_idle_entry, state_ntn_idle_run, state_ntn_idle_exit,
				 &states[STATE_NTN],
				 NULL),
	[STATE_NTN_BACK_SCAN] =
		SMF_CREATE_STATE(state_ntn_back_scan_entry, state_ntn_back_scan_run, state_ntn_back_scan_exit,
				 &states[STATE_NTN],
				 NULL),
};

static void handle_gnss_timeout_work_fn(struct k_work *work)
{
	/* GNSS timeout */
	ntn_msg_publish(GNSS_TIMEOUT);
}

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
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_NO_SUITABLE_CELL) {
			LOG_WRN("Not registered, check rejection cause");
			network_status_notify(NETWORK_NO_SUITABLE_CELL);
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

		default:
			break;
		}

		break;
	case LTE_LC_EVT_RRC_UPDATE:
		if (evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED) {
			LOG_DBG("LTE_LC_RRC_MODE_CONNECTED");
		}
		else if (evt->rrc_mode == LTE_LC_RRC_MODE_IDLE) {
			LOG_DBG("LTE_LC_RRC_MODE_IDLE");
		}

		break;
	case LTE_LC_EVT_CELL_UPDATE:
		struct lte_lc_cell cell_info = evt->cell;
		LOG_DBG("LTE_LC_EVT_CELL_UPDATE, id: %u", cell_info.id);
		LOG_DBG("LTE_LC_EVT_CELL_UPDATE, tac: %u", cell_info.tac);

		break;
	case LTE_LC_EVT_MODEM_EVENT:
		/* If a reset loop happens in the field, it should not be necessary
		 * to perform any action. The modem will try to re-attach to the LTE network after
		 * the 30-minute block.
		 */
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

static void gnss_event_handler(int event)
{
	int err;

	switch (event) {
	case NRF_MODEM_GNSS_EVT_PVT:
		/* Schedule work to handle PVT data in thread context */
		err = k_work_submit(&gnss_location_work);
		if (err < 0) {
			LOG_ERR("Failed to submit GNSS location work, error: %d", err);
		}

		break;
	case NRF_MODEM_GNSS_EVT_FIX:
		LOG_DBG("NRF_MODEM_GNSS_EVT_FIX");

		break;
	case NRF_MODEM_GNSS_EVT_BLOCKED:
		LOG_WRN("NRF_MODEM_GNSS_EVT_BLOCKED");

		break;
	case NRF_MODEM_GNSS_EVT_SLEEP_AFTER_TIMEOUT:
		LOG_ERR("NRF_MODEM_GNSS_EVT_SLEEP_AFTER_TIMEOUT");
		/* Schedule work to set IDLE state in thread context */
		err = k_work_submit(&gnss_timeout_work);
		if (err < 0) {
			LOG_ERR("Failed to submit gnss_timeout_work, error: %d", err);
		}

		break;
	default:
		LOG_DBG("Unknown GNSS event: %d", event);

		break;
	}
}


static void ntn_event_handler(const struct ntn_evt *evt)
{
	switch (evt->type) {
	case NTN_EVT_LOCATION_REQUEST:
		LOG_DBG("NTN location requested: %s, accuracy: %d m",
			evt->location_request.requested ? "true" : "false",
			evt->location_request.accuracy);

		ntn_msg_publish(NTN_LOCATION_REQUEST);

		break;
	default:

		break;
	}
}

static void publish_last_pvt(const struct nrf_modem_gnss_pvt_data_frame *pvt)
{
	int err;
	struct ntn_msg msg = {
		.type = LOCATION_SEARCH_DONE,
		.pvt = *pvt
	};

	err = zbus_chan_pub(&NTN_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("Failed to publish last PVT message, error: %d", err);
	}
}

static void apply_gnss_time(const struct nrf_modem_gnss_pvt_data_frame *pvt_data)
{
	int err;
	struct tm gnss_time = {
		.tm_year = pvt_data->datetime.year - 1900,
		.tm_mon = pvt_data->datetime.month - 1,
		.tm_mday = pvt_data->datetime.day,
		.tm_hour = pvt_data->datetime.hour,
		.tm_min = pvt_data->datetime.minute,
		.tm_sec = pvt_data->datetime.seconds,
	};

	err = date_time_set(&gnss_time);
	if (err) {
		LOG_ERR("Failed to apply GNSS time, error: %d", err);
	}
}


static void gnss_location_work_handler(struct k_work *work)
{
	int err;
	struct nrf_modem_gnss_pvt_data_frame pvt_data;

	/* Read PVT data in thread context */
	err = nrf_modem_gnss_read(&pvt_data, sizeof(pvt_data), NRF_MODEM_GNSS_DATA_PVT);
	if (err != 0) {
		LOG_ERR("Failed to read GNSS data nrf_modem_gnss_read(), err: %d", err);

		return;
	}

	if (pvt_data.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
		LOG_DBG("Got valid GNSS location: lat: %f, lon: %f, alt: %f",
			(double)pvt_data.latitude,
			(double)pvt_data.longitude,
			(double)pvt_data.altitude);
		apply_gnss_time(&pvt_data);
		publish_last_pvt(&pvt_data);
	}

	/* Log SV (Satellite Vehicle) data */
	for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
		if (pvt_data.sv[i].sv == 0) {
		/* SV not valid, skip */
		continue;
		}

		LOG_DBG("SV: %3d C/N0: %4.1f el: %2d az: %3d signal: %d in fix: %d unhealthy: %d",
		pvt_data.sv[i].sv,
		pvt_data.sv[i].cn0 * 0.1,
		pvt_data.sv[i].elevation,
		pvt_data.sv[i].azimuth,
		pvt_data.sv[i].signal,
		pvt_data.sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX ? 1 : 0,
		pvt_data.sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_UNHEALTHY ? 1 : 0);
	}
}

static void sample_network_quality(void)
{
	int ret;
	struct network_msg msg = {
		.type = NETWORK_QUALITY_SAMPLE_RESPONSE,
		.timestamp = k_uptime_get()
	};

	ret = date_time_now(&msg.timestamp);
	if (ret != 0 && ret != -ENODATA) {
		LOG_ERR("date_time_now, error: %d", ret);
		SEND_FATAL_ERROR();
		return;
	}

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

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set, error: %d", err);

		return;
	}
	return 0;
}

/* State handlers */

static void state_running_entry(void *obj)
{
	int err;

	ARG_UNUSED(obj);

	LOG_DBG("state_running_entry");

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("Failed to initialize the modem library, error: %d", err);

		return;
	}

	lte_lc_register_handler(lte_lc_evt_handler);

	/* Register handler for default PDP context. */
	err = lte_lc_pdn_default_ctx_events_enable();
	if (err) {
		LOG_ERR("lte_lc_pdn_default_ctx_events_enable, error: %d", err);

		return;
	}

	k_work_init(&gnss_location_work, gnss_location_work_handler);
	k_work_init(&gnss_timeout_work, handle_gnss_timeout_work_fn);
	nrf_modem_gnss_event_handler_set(gnss_event_handler);
	ntn_register_handler(ntn_event_handler);

#if defined(APP_NETWORK_TN_SOFTSIM)
	lte_lc_uicc tn_uicc = LTE_LC_UICC_SOFTSIM;
else
	lte_lc_uicc tn_uicc = LTE_LC_UICC_PHYSICAL;
#endif

	// for tn I should be able to select soft (if LEO) or phy sim
	struct lte_lc_cellular_profile tn_profile = {
			.id = 0,
			.act = LTE_LC_ACT_LTEM | LTE_LC_ACT_NBIOT,
			.uicc = tn_uicc,
		};

	struct lte_lc_cellular_profile ntn_profile = {
			.id = 1,
			.act = LTE_LC_ACT_NTN,
			.uicc = LTE_LC_UICC_PHYSICAL,
		};

	err = lte_lc_cellular_profile_configure(&tn_profile);
		if (err) {
			LOG_ERR("Failed to set TN profile, error: %d", err);

			return;
		}

	err = lte_lc_cellular_profile_configure(&ntn_profile);
		if (err) {
			LOG_ERR("Failed to set NTN profile, error: %d", err);

			return;
		}

	LOG_DBG("Network module started");
}

static enum smf_state_result state_running_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	if (&NETWORK_CHAN == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_DISCONNECTED:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED]);

			return SMF_EVENT_HANDLED;
		case NETWORK_UICC_FAILURE:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);

			return SMF_EVENT_HANDLED;
		case NETWORK_QUALITY_SAMPLE_REQUEST:
			sample_network_quality();

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

	LOG_DBG("state_disconnected_entry");
}

static enum smf_state_result state_disconnected_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	if (&NETWORK_CHAN == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_CONNECTED:
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED]);

			return SMF_EVENT_HANDLED;
		case NETWORK_NO_SUITABLE_CELL:
			smf_set_state(SMF_CTX(state_object), &states[STATE_NTN]);

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

	err = lte_lc_connect_async(lte_lc_evt_handler);
	if (err) {
		LOG_ERR("lte_lc_connect_async, error: %d", err);

		return;
	}
}

static enum smf_state_result state_disconnected_searching_run(void *obj)
{
	int err;
	struct network_state_object const *state_object = obj;

	if (&NETWORK_CHAN == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_CONNECT:
			return SMF_EVENT_HANDLED;
		case NETWORK_SEARCH_STOP: __fallthrough;
		case NETWORK_DISCONNECT:
			err = network_disconnect();
			if (err) {
				LOG_ERR("network_disconnect, error: %d", err);
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

static enum smf_state_result state_disconnected_idle_run(void *obj)
{
	int err;
	struct network_state_object const *state_object = obj;

	if (&NETWORK_CHAN == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_DISCONNECT:
			return SMF_EVENT_HANDLED;
		case NETWORK_CONNECT:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_SEARCHING]);

			return SMF_EVENT_HANDLED;
		case NETWORK_SYSTEM_MODE_SET_LTEM:
			err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_LTEM_GPS,
						     LTE_LC_SYSTEM_MODE_PREFER_AUTO);
			if (err) {
				LOG_ERR("lte_lc_system_mode_set, error: %d", err);
				SEND_FATAL_ERROR();
			}

			return SMF_EVENT_HANDLED;
		case NETWORK_SYSTEM_MODE_SET_NBIOT:
			err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_NBIOT_GPS,
						     LTE_LC_SYSTEM_MODE_PREFER_AUTO);
			if (err) {
				LOG_ERR("lte_lc_system_mode_set, error: %d", err);
				SEND_FATAL_ERROR();
			}

			return SMF_EVENT_HANDLED;
		case NETWORK_SYSTEM_MODE_SET_LTEM_NBIOT:
			err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS,
						     LTE_LC_SYSTEM_MODE_PREFER_AUTO);
			if (err) {
				LOG_ERR("lte_lc_system_mode_set, error: %d", err);
				SEND_FATAL_ERROR();
			}

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

	if (&NETWORK_CHAN == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_QUALITY_SAMPLE_REQUEST:
			LOG_DBG("Sampling network quality data");
			sample_network_quality();

			return SMF_EVENT_HANDLED;
		case NETWORK_DISCONNECT:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTING]);

			return SMF_EVENT_HANDLED;
		default:
			break;
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
		return;
	}
}

static enum smf_state_result state_disconnecting_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	if (&NETWORK_CHAN == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		if (msg.type == NETWORK_DISCONNECTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_ntn_entry(void *obj)
{
	int err;

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set, error: %d", err);

		return;
	}

	/* Select physical SIM for NTN mode */
	err = nrf_modem_at_printf("AT%%CSUS=0");
	if (err) {
		LOG_ERR("Failed to select physical SIM, error: %d", err);
		return;
	}

#if defined(CONFIG_APP_NTN_BANDLOCK_ENABLE)
	err = nrf_modem_at_printf("AT%%XBANDLOCK=1,,\"%i\"", CONFIG_APP_NTN_BANDLOCK);
	if (err) {
		LOG_ERR("Failed to set NTN band lock, error: %d", err);

		return;
	}
#endif

#if defined(CONFIG_APP_NTN_CHANNEL_SELECT_ENABLE)
	err = nrf_modem_at_printf("AT%%CHSELECT=2,14,%i", CONFIG_APP_NTN_CHANNEL_SELECT);
	if (err) {
		LOG_ERR("Failed to set NTN channel, error: %d", err);

		return;
	}
#endif

	return;
}

static enum smf_state_result state_ntn_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	if (&NETWORK_CHAN == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_TN_CELL_FOUND:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED]);

			return SMF_EVENT_HANDLED;
		case BACKGROUND_SCAN:
			smf_set_state(SMF_CTX(state_object), &states[STATE_NTN_BACK_SCAN]);

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_ntn_exit(void *obj)
{
	int err;

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set, error: %d", err);

		return;
	}

#if defined(APP_NETWORK_TN_SOFTSIM)
	err = nrf_modem_at_printf("AT%%CSUS=2");
	if (err) {
		LOG_ERR("Failed to select softsim, error: %d", err);
		return;
	}
#endif

	/* Remove bandlock */
	err = nrf_modem_at_printf("AT%%XBANDLOCK=0");
	if (err) {
		LOG_ERR("Failed to set remove NTN band lock, error: %d", err);

		return;
	}

	/* Remove channel lock */
	err = nrf_modem_at_printf("AT%%CHSELECT=0");
	if (err) {
		LOG_ERR("Failed to set NTN channel, error: %d", err);

		return;
	}

	err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_LTEM_NBIOT,
				     LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	if (err) {
		LOG_ERR("lte_lc_system_mode_set, error: %d", err);

		return;
	}

}

static void state_ntn_connect_entry(void *obj)
{
	int err;
	uint32_t location_validity_time;
	uint64_t current_time = k_uptime_get();
	struct network_state_object const *state_object = obj;

	LOG_DBG("%s", __func__);

	// If LEO and no scheduled pass, compute SGP4 and return
	// FIX
#if defined(CONFIG_APP_NETWORK_NTN_LEO)
	if (state_object->pass_scheduled) {
		LOF_INF("Pass scheduled already");
	} else {
		compute_sgp4();
		rechedule_pass();
		smf_set_state(SMF_CTX(state_object), &states[STATE_IDLE]);
	}
#endif

	if (state->location_validity_end_time > current_time) {
		location_validity_time =
			(uint32_t)(state->location_validity_end_time - current_time) / MSEC_PER_SEC;
	} else {
		location_validity_time = 1;
	}

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set, error: %d", err);

		return err;
	}

	/* Configure NTN system mode */
	err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_NTN_NBIOT, LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	if (err) {
		LOG_ERR("Failed to set NTN system mode, error: %d", err);

		return err;
	}

	/* Configure location using latest GNSS data */
	err = ntn_location_set((double)state->last_pvt.latitude,
				(double)state->last_pvt.longitude,
				(float)state->last_pvt.altitude,
				location_validity_time);
	if (err) {
		LOG_ERR("Failed to set location, error: %d", err);

		return err;
	}

	configure_periodic_search();

	/* Configure SIB32 and start monitoring */
	err = nrf_modem_at_printf("AT%%SIBCONFIG=32,0");
	if (err) {
		LOG_ERR("Failed to configure SIB32, error: %d", err);
		return err;
	}
	LOG_INF("SIB32 configured successfully");
	at_monitor_resume(&sib32_monitor);

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_LTE);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set, error: %d\n", err);

		return err;
	}

	return 0;
}

static enum smf_state_result state_ntn_connect_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	if (&NETWORK_CHAN == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NTN_LOCATION_REQUEST:
			smf_set_state(SMF_CTX(state_object), &states[STATE_NTN_GNSS]);

			return SMF_EVENT_HANDLED;
		case CLOUD_CONNECTED:
			// FIX nrfcloud_location_send

			return SMF_EVENT_HANDLED;
		// FIX go to idle upon data sent or on timeout
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_ntn_connect_exit(void *obj)
{
	int err;

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set, error: %d", err);

		return;
	}

	return;

}

static void state_ntn_gnss_entry(void *obj)
{
	int err;
	int periodic_fix_retry = 180;

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set, error: %d", err);

		return;
	}

	err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_GPS,
				     LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	if (err) {
		LOG_ERR("Failed to set GNSS system mode, error: %d", err);

		return;
	}

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS);
	if (err) {
		LOG_ERR("Failed to activate GNSS fun mode, error: %d", err);

		return;
	}

	/* Set GNSS to single fix mode */
	err = nrf_modem_gnss_fix_interval_set(0);
	if (err) {
		LOG_ERR("Failed to set GNSS fix interval, error: %d", err);
	}

	/* Set GNSS fix timeout to 180 seconds */
	err = nrf_modem_gnss_fix_retry_set(periodic_fix_retry);
	if (err) {
		LOG_ERR("Failed to set GNSS fix retry, error: %d", err);
	}

	err = nrf_modem_gnss_start();
	if (err) {
		LOG_ERR("Failed to start GNSS, error: %d", err);
	}

	return;
}

static enum smf_state_result state_ntn_gnss_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	if (&NETWORK_CHAN == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case LOCATION_SEARCH_DONE:
			// FIX parse the location data
			memcpy(&state->last_pvt, &msg->pvt, sizeof(state->last_pvt));
			state->location_validity_end_time =
				k_uptime_get() +
				CONFIG_APP_NTN_LOCATION_VALIDITY_TIME_SECONDS * MSEC_PER_SEC;
			smf_set_state(SMF_CTX(state_object), &states[STATE_NTN_CONNECT]);

			return SMF_EVENT_HANDLED;
		case GNSS_TIMEOUT:
			smf_set_state(SMF_CTX(state_object), &states[STATE_NTN_IDLE]);

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_ntn_gnss_exit(void *obj)
{
	int err;

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	err = nrf_modem_gnss_stop();
	if (err) {
		LOG_ERR("Failed to stop GNSS, error: %d", err);
	}

	/* Set modem to CFUN=30 mode when exiting GNSS state */
	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_DEACTIVATE_GNSS);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set, error: %d", err);

		return err;
	}

}

static void state_ntn_idle_entry(void *obj)
{
	int err;

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

}

static enum smf_state_result state_ntn_idle_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	if (&NETWORK_CHAN == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		// FIX what should idle do?
		case NETWORK_DISCONNECTED:
			// smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED]);

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_ntn_idle_exit(void *obj)
{
	int err;

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

}

static void state_ntn_back_scan_entry(void *obj)
{
	int err;

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	// FIX perform_back_ground_scan

}

static enum smf_state_result state_ntn_back_scan_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	if (&NETWORK_CHAN == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		// FIX what should back_ground_scan do?
		case NETWORK_DISCONNECTED:
			// smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED]);

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_ntn_back_scan_exit(void *obj)
{
	int err;

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

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
