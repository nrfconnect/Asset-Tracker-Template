/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/smf.h>
#include <modem/lte_lc.h>
#include <modem/pdn.h>
#include <date_time.h>
#include <modem/nrf_modem_lib.h>
#include <nrf_modem_at.h>
#include <modem/at_monitor.h>
#include <nrf_modem_gnss.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <errno.h>
#include <net/nrf_cloud.h>
#include <net/nrf_cloud_coap.h>
#include <net/nrf_cloud_rest.h>
#include <net/nrf_provisioning.h>
#include <nrf_cloud_coap_transport.h>
#include <zephyr/net/coap.h>
#include <app_version.h>

#include "app_common.h"
#include "ntn.h"
#include "button.h"

LOG_MODULE_REGISTER(ntn, CONFIG_APP_NTN_LOG_LEVEL);

/* AT monitor for network notifications.
 * The monitor is needed to receive notification when in the case where the modem has been
 * put into offline mode while keeping registration context.
 * In this case, the modem will send a +CEREG notification with status 1 or 5 when NTN is
 * re-enabled. The LTE link controller does not forward this because it is equal to the previous
 * registration status. To work around this, we monitor the +CEREG notification and forward it
 * to the NTN module when offline-while-keeping-registration mode is enabled.
 */
AT_MONITOR(cereg_monitor, "CEREG", cereg_mon, PAUSED);

/* Define channels provided by this module */
ZBUS_CHAN_DEFINE(NTN_CHAN,
		 struct ntn_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(ntn_subscriber);

/* Observe NTN channel */
ZBUS_CHAN_ADD_OBS(NTN_CHAN, ntn_subscriber, 0);
ZBUS_CHAN_ADD_OBS(BUTTON_CHAN, ntn_subscriber, 0);

/* Define maximum message size that the ntn_subscriber will receive */
#define MAX_MSG_SIZE	MAX(sizeof(struct ntn_msg), sizeof(struct button_msg))

/* State machine states */
enum ntn_module_state {
	/* The module is initialized and is running */
	STATE_RUNNING,
		/* In TN mode, the module is connecting to terrestrial network and establishing
		 * a CoAP over DTLS connection to nRF Cloud.
		 */
		STATE_TN,
		/* In GNSS mode, the module is searching for GNSS satellites and acquiring a GNSS
		 * fix. The location is to be used when searching for a suitable cell in NTN mode.
		 */
		STATE_GNSS,
		/* In NTN mode, the module attempts to connect to an suitable NTN cell.
		 * Once a suitable cell is found, the module sends GNSS location data to nRF Cloud,
		 * resuming the existing CoAP connection to nRF Cloud.
		 */
		STATE_NTN,
		/* In IDLE mode, the module is not connected to any network and is waiting for a
		 * timeout or button press to transition to another mode.
		 */
		STATE_IDLE,
};

/* State object */
struct ntn_state_object {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	uint8_t msg_buf[MAX_MSG_SIZE];
	struct k_timer ntn_timer;
	struct nrf_modem_gnss_pvt_data_frame last_pvt;
};

static struct k_work timer_work;
static struct k_work gnss_location_work;

/* Forward declarations */

static void gnss_event_handler(int event);
static void lte_lc_evt_handler(const struct lte_lc_evt *const evt);
static void pdn_event_handler(uint8_t cid, enum pdn_event event, int reason);
static void ntn_msg_publish(enum ntn_msg_type type);

static void state_running_entry(void *obj);
static enum smf_state_result state_running_run(void *obj);
static void state_tn_entry(void *obj);
static enum smf_state_result state_tn_run(void *obj);
static void state_tn_exit(void *obj);
static void state_gnss_entry(void *obj);
static enum smf_state_result state_gnss_run(void *obj);
static void state_gnss_exit(void *obj);
static void state_ntn_entry(void *obj);
static enum smf_state_result state_ntn_run(void *obj);
static void state_ntn_exit(void *obj);
static void state_idle_entry(void *obj);
static enum smf_state_result state_idle_run(void *obj);

/* State machine definition */
static const struct smf_state states[] = {
	[STATE_RUNNING] = SMF_CREATE_STATE(state_running_entry, state_running_run, NULL,
				NULL, &states[STATE_TN]),
	[STATE_TN] = SMF_CREATE_STATE(state_tn_entry, state_tn_run, state_tn_exit,
				&states[STATE_RUNNING], NULL),
	[STATE_GNSS] = SMF_CREATE_STATE(state_gnss_entry, state_gnss_run, state_gnss_exit,
				&states[STATE_RUNNING], NULL),
	[STATE_NTN] = SMF_CREATE_STATE(state_ntn_entry, state_ntn_run, state_ntn_exit,
				&states[STATE_RUNNING], NULL),
	[STATE_IDLE] = SMF_CREATE_STATE(state_idle_entry, state_idle_run, NULL,
				&states[STATE_RUNNING], NULL),
};

/* Event handlers */

static void timer_work_handler(struct k_work *work)
{
	ntn_msg_publish(NTN_TIMEOUT);
}

/* Timer callback for NTN mode timeout */
static void ntn_timer_handler(struct k_timer *timer)
{
	k_work_submit(&timer_work);
}

static void lte_lc_evt_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if (evt->nw_reg_status == LTE_LC_NW_REG_UICC_FAIL) {
			LOG_ERR("No SIM card detected!");
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_NOT_REGISTERED) {
			LOG_WRN("Not registered, check rejection cause");
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_NO_SUITABLE_CELL) {
			LOG_DBG("LTE_LC_NW_REG_NO_SUITABLE_CELL");
			ntn_msg_publish(NETWORK_NO_SUITABLE_CELL);
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) {
			LOG_DBG("LTE_LC_NW_REG_REGISTERED_HOME");
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {
			LOG_DBG("LTE_LC_NW_REG_REGISTERED_ROAMING");
		}

		break;
	case LTE_LC_EVT_MODEM_EVENT:
		if (evt->modem_evt.type == LTE_LC_MODEM_EVT_RESET_LOOP) {
			LOG_WRN("The modem has detected a reset loop!");
		} else if (evt->modem_evt.type == LTE_LC_MODEM_EVT_LIGHT_SEARCH_DONE) {
			LOG_DBG("LTE_LC_MODEM_EVT_LIGHT_SEARCH_DONE");
		}

		break;
	case LTE_LC_EVT_RRC_UPDATE:
		if (evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED) {
			LOG_DBG("LTE_LC_RRC_MODE_CONNECTED");

		} else if (evt->rrc_mode == LTE_LC_RRC_MODE_IDLE) {
			LOG_DBG("LTE_LC_RRC_MODE_IDLE");
		}

		break;
	default:
		break;
	}
}

static void pdn_event_handler(uint8_t cid, enum pdn_event event, int reason)
{
	switch (event) {
#if CONFIG_PDN_ESM_STRERROR
	case PDN_EVENT_CNEC_ESM:
		LOG_DBG("Event: PDP context %d, %s", cid, pdn_esm_strerror(reason));

		break;
#endif
	case PDN_EVENT_ACTIVATED:
		LOG_DBG("PDN_EVENT_ACTIVATED");
		ntn_msg_publish(NETWORK_CONNECTED);

		break;
	case PDN_EVENT_NETWORK_DETACH:
		LOG_DBG("PDN_EVENT_NETWORK_DETACH");

		break;
	case PDN_EVENT_DEACTIVATED:
		LOG_DBG("PDN_EVENT_DEACTIVATED");

		break;
	case PDN_EVENT_CTX_DESTROYED:
		LOG_DBG("PDN_EVENT_CTX_DESTROYED");

		break;
	default:
		LOG_ERR("Unexpected PDN event: %d", event);

		break;
	}
}

static void gnss_event_handler(int event)
{
	switch (event) {
	case NRF_MODEM_GNSS_EVT_PVT:
		/* Schedule work to handle PVT data in thread context */
		k_work_submit(&gnss_location_work);

		break;
	/* TODO: add handling for GNSS_SEARCH_FAILED, see mosh gnss.c */
	default:
		break;
	}
}

static void ntn_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("NTN watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));
}

/* Helper function to publish messages on the internal NTN channel */
static void ntn_msg_publish(enum ntn_msg_type type)
{
	int err;
	struct ntn_msg msg = {
		.type = type
	};

	err = zbus_chan_pub(&NTN_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("Failed to publish NTN message, error: %d", err);

		return;
	}
}

/* Publish last PVT data on the internal NTN channel */
static void publish_last_pvt(const struct nrf_modem_gnss_pvt_data_frame *pvt)
{
	int err;
	struct ntn_msg msg = {
		.type = NTN_LOCATION_SEARCH_DONE,
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

/* Helper functions */

static void cereg_mon(const char *notif)
{
	enum lte_lc_nw_reg_status status = atoi(notif + (sizeof("+CEREG: ") - 1));

	if ((status == LTE_LC_NW_REG_REGISTERED_ROAMING) ||
	    (status == LTE_LC_NW_REG_REGISTERED_HOME)) {
		LOG_DBG("Network registration status: %s",
			status == LTE_LC_NW_REG_REGISTERED_ROAMING ? "ROAMING" : "HOME");
		ntn_msg_publish(NETWORK_CONNECTED);
		LOG_DBG("Stop monitoring incoming CEREG Notifications");
		at_monitor_pause(&cereg_monitor);
	}
}

static int set_ntn_offline_mode(void)
{
	int err;

	/* Set modem to offline mode without losing registration */
	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set, error: %d", err);

		return err;
	}

	return 0;
}

static int set_ntn_active_mode(struct ntn_state_object *state)
{
	int err;
	enum lte_lc_func_mode mode;
	bool ntn_initialized = false;

	err = lte_lc_func_mode_get(&mode);
	if (err) {
		LOG_ERR("Failed to get LTE function mode, error: %d", err);

		return err;
	}

	/* If needed, go offline to be able to set NTN system mode */
	switch (mode) {
	case LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG: __fallthrough;
	case LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG_UICC_ON:;
		ntn_initialized = true;

		break;
	case LTE_LC_FUNC_MODE_OFFLINE: __fallthrough;
	case LTE_LC_FUNC_MODE_POWER_OFF: __fallthrough;
	default:
		err = lte_lc_offline();
		if (err) {
			LOG_ERR("lte_lc_offline, error: %d", err);

			return err;
		}

		break;
	}

	/* Check if we are in offline-while-keeping-registration mode. If so, it indicates that
	 * the modem has already been able to register to an NTN network, which in turn means
	 * that the NTN parameters are already configured.
	 */
	if (ntn_initialized) {
		/* Start monitoring incoming CEREG notifications temporarily to receive +CEREG
		 * notifications when in offline-while-keeping-registration mode.
		 */
		LOG_DBG("Start monitoring incoming CEREG Notifications");
		at_monitor_resume(&cereg_monitor);

		/* Configure NTN system mode */
		err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_NTN_NBIOT,
					     LTE_LC_SYSTEM_MODE_PREFER_AUTO);
		if (err) {
			LOG_ERR("Failed to set NTN system mode, error: %d", err);

			return err;
		}

		/* Configure location using latest GNSS data */
		err = nrf_modem_at_printf("AT%%LOCATION=2,\"%f\",\"%f\",\"%f\",0,0",
					(double)state->last_pvt.latitude,
					(double)state->last_pvt.longitude,
					(double)state->last_pvt.altitude);
		if (err) {
			LOG_ERR("Failed to set AT%%LOCATION, error: %d", err);

			return err;
		}

		LOG_DBG("NTN initialized, activating LTE functional mode");

		err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_LTE);
		if (err) {
			LOG_ERR("lte_lc_func_mode_set, error: %d", err);

			return err;
		}
	} else {
		/* Set NTN SIM profile.
		 * 2: Configure cellular profile
		 * 0: Cellular profile index
		 * 4: Access technology: Satellite E-UTRAN (NB-S1 mode)
		 * 0: SIM slot, physical SIM
		*/
		err = nrf_modem_at_printf("AT%%CELLULARPRFL=2,0,4,0");
		if (err) {
			LOG_ERR("Failed to set modem NTN profile, error: %d", err);

			return err;
		}

		/* Set TN SIM profile for LTE-M
		 * 2: Configure cellular profile
		 * 1: Cellular profile index
		 * 1: Access technology: LE-UTRAN (WB-S1 mode), LTE-M
		 * 0: SIM slot, physical SIM
		*/
		err = nrf_modem_at_printf("AT%%CELLULARPRFL=2,1,1,0");
		if (err) {
			LOG_ERR("Failed to set modem TN profile, error: %d", err);

			return err;
		}

#if defined(CONFIG_APP_NTN_DISABLE_EPCO)
		/* Disable ePCO */
		err = nrf_modem_at_printf("AT%%XEPCO=0");
		if (err) {
			LOG_ERR("Failed to set XEPCO off, error: %d", err);

			return err;
		}
#endif

		/* Configure NTN system mode */
		err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_NTN_NBIOT,
					     LTE_LC_SYSTEM_MODE_PREFER_AUTO);
		if (err) {
			LOG_ERR("Failed to set NTN system mode, error: %d", err);

			return err;
		}

		/* Configure location using latest GNSS data */
		err = nrf_modem_at_printf("AT%%LOCATION=2,\"%f\",\"%f\",\"%f\",0,0",
					(double)state->last_pvt.latitude,
					(double)state->last_pvt.longitude,
					(double)state->last_pvt.altitude);
		if (err) {
			LOG_ERR("Failed to set AT%%LOCATION, error: %d", err);

			return err;
		}

#if defined(CONFIG_APP_NTN_BANDLOCK_ENABLE)
		/* Set NTN band lock */
		err = nrf_modem_at_printf("AT%%XBANDLOCK=2,,\"%i\"", CONFIG_APP_NTN_BANDLOCK);
		if (err) {
			LOG_ERR("Failed to set NTN band lock, error: %d", err);

			return err;
		}
#endif

#if defined(CONFIG_APP_NTN_CHANNEL_SELECT_ENABLE)
		err = nrf_modem_at_printf("AT%%CHSELECT=1,14,%i", CONFIG_APP_NTN_CHANNEL_SELECT);
		if (err) {
			LOG_ERR("Failed to set NTN channel, error: %d", err);

			return err;
		}
#endif

		/*
		 * Modem is activating AT+CPSMS via CONFIG_LTE_LC_PSM_MODULE=y.
		 * Cast AT+CPSMS=0 to deactivate legacy PSM.
		 * CFUN=45 + legacy PSM is has bugs in mfw ntn-0.5.0, fixed in mfw ntn-0.5.1.
		 */
		err = nrf_modem_at_printf("AT+CPSMS=0");
		if (err) {
			LOG_ERR("Failed to set AT+CPSMS=0, error: %d", err);

			return err;
		}

		LOG_DBG("NTN not initialized, using lte_lc_connect_async to connect to network");

		err = lte_lc_connect_async(lte_lc_evt_handler);
		if (err) {
			LOG_ERR("lte_lc_connect_async, error: %d\n", err);

			return err;
		}
	}

	return 0;
}

static int set_gnss_active_mode(struct ntn_state_object *state)
{
	int err;
	enum lte_lc_func_mode mode;

	err = lte_lc_func_mode_get(&mode);
	if (err) {
		LOG_ERR("Failed to get LTE function mode, error: %d", err);

		return err;
	}

	if ((mode != LTE_LC_FUNC_MODE_OFFLINE) && (mode != LTE_LC_FUNC_MODE_POWER_OFF)) {
		/* Go offline to be able to set GNSS system mode */
		err = lte_lc_offline();
		if (err) {
			LOG_ERR("lte_lc_offline, error: %d", err);

			return err;
		}
	}

	/* Configure GNSS system mode */
	err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_GPS,
					LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	if (err) {
		LOG_ERR("Failed to set GNSS system mode, error: %d", err);

		return err;
	}

	/* Activate GNSS functional mode */
	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS);
	if (err) {
		LOG_ERR("Failed to activate GNSS fun mode, error: %d", err);

		return err;
	}

	/* Set GNSS to single fix mode */
	err = nrf_modem_gnss_fix_interval_set(0);
	if (err) {
		LOG_ERR("Failed to set GNSS fix interval, error: %d", err);
	}

	/* Set GNSS fix timeout to 180 seconds */
	err = nrf_modem_gnss_fix_retry_set(180);
	if (err) {
		LOG_ERR("Failed to set GNSS fix retry, error: %d", err);
	}

	err = nrf_modem_gnss_start();
	if (err) {
		LOG_ERR("Failed to start GNSS, error: %d", err);
	}

	return 0;
}

static int set_gnss_inactive_mode(void)
{
	int err;

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

	return 0;
}

/* State handlers */

static void state_running_entry(void *obj)
{
	int err;
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_DBG("%s", __func__);

	k_work_init(&timer_work, timer_work_handler);
	k_work_init(&gnss_location_work, gnss_location_work_handler);
	k_timer_init(&state->ntn_timer, ntn_timer_handler, NULL);

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("Failed to initialize the modem library, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	/* Register GNSS event handler */
	nrf_modem_gnss_event_handler_set(gnss_event_handler);

	/* Register LTE event handler */
	lte_lc_register_handler(lte_lc_evt_handler);

	/* Register handler for default PDP context 0. */
	err = pdn_default_ctx_cb_reg(pdn_event_handler);
	if (err) {
		LOG_ERR("pdn_default_ctx_cb_reg, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	/* Init nrfcloud coap */
	err = nrf_cloud_coap_init();
	if (err) {
		LOG_ERR("nrf_cloud_coap_init, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static enum smf_state_result state_running_run(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_DBG("%s", __func__);

	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		if (msg->type == SET_NTN_IDLE) {
			smf_set_state(SMF_CTX(state), &states[STATE_IDLE]);
		}
	} else if (state->chan == &BUTTON_CHAN) {
		struct button_msg *msg = (struct button_msg *)state->msg_buf;

		if (msg->type == BUTTON_PRESS_LONG) {
			smf_set_state(SMF_CTX(state), &states[STATE_TN]);
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_tn_entry(void *obj)
{
	int err;

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	/* Offline mode	*/
	err = lte_lc_power_off();
	if (err) {
		LOG_ERR("lte_lc_power_off, error: %d", err);

		return;
	}

	err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_LTEM, LTE_LC_SYSTEM_MODE_PREFER_LTEM);
	if (err) {
		LOG_ERR("lte_lc_system_mode_set, error: %d", err);

		return;
	}

	/* Connect to network */
	LOG_DBG("TN mode, using lte_lc_connect_async to connect to network");

	err = lte_lc_connect_async(lte_lc_evt_handler);
	if (err) {
		LOG_ERR("lte_lc_connect_async, error: %d\n", err);

		return;
	}
}

static enum smf_state_result state_tn_run(void *obj)
{
	int err;
	struct ntn_state_object *state = (struct ntn_state_object *)obj;
	char buf[NRF_CLOUD_CLIENT_ID_MAX_LEN];

	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		if (msg->type == NETWORK_CONNECTED) {
			LOG_DBG("Received NETWORK_CONNECTED, connecting to nRF Cloud CoAP");

			err = nrf_cloud_client_id_get(buf, sizeof(buf));
			if (err) {
				LOG_ERR("nrf_cloud_client_id_get, error: %d, cannot continue", err);

				return SMF_EVENT_HANDLED;
			}

			LOG_INF("Connecting to nRF Cloud CoAP using client ID: %s", buf);

			err = nrf_cloud_coap_connect(APP_VERSION_STRING);
			if (err == -EACCES || err == -ENOEXEC || err == -ECONNREFUSED) {
				LOG_WRN("nrf_cloud_coap_connect, error: %d", err);
				LOG_WRN("nRF Cloud CoAP connection failed, unauthorized or invalid credentials");

				return SMF_EVENT_HANDLED;
			} else if (err < 0) {
				LOG_WRN("nRF Cloud CoAP connection refused");

				return SMF_EVENT_HANDLED;
			}

			LOG_INF("Cloud connection established via TN network");

			/* Pause the CoAP connection to save the DTLS CID and resume it later
			 * when transitioning to NTN mode.
			 */
			err = nrf_cloud_coap_pause();
			if ((err < 0) && (err != -EBADF)) {
				/* -EBADF means cloud was disconnected */
				LOG_ERR("Error pausing connection: %d", err);

				return SMF_EVENT_HANDLED;
			}

			LOG_INF("CoAP connection paused");

			smf_set_state(SMF_CTX(state), &states[STATE_IDLE]);

			return SMF_EVENT_HANDLED;
		} else if (msg->type == NETWORK_NO_SUITABLE_CELL) {
			LOG_INF("Out of LTE coverage, going to idle state");
			smf_set_state(SMF_CTX(state), &states[STATE_IDLE]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_tn_exit(void *obj)
{
	int err;

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	err = lte_lc_offline();
	if (err) {
		LOG_ERR("lte_lc_offline, error: %d", err);

		return;
	}
}

static void state_idle_entry(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_DBG("%s", __func__);

	k_timer_start(&state->ntn_timer,
		      K_MINUTES(CONFIG_APP_NTN_TIMER_TIMEOUT_MINUTES),
		      K_NO_WAIT);
}

static enum smf_state_result state_idle_run(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_DBG("%s", __func__);

	if (state->chan == &BUTTON_CHAN) {
		struct button_msg *msg = (struct button_msg *)state->msg_buf;

		if (msg->type == BUTTON_PRESS_SHORT) {
			smf_set_state(SMF_CTX(state), &states[STATE_GNSS]);

			return SMF_EVENT_HANDLED;
		}
	} else if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		if (msg->type == NTN_TIMEOUT) {
			smf_set_state(SMF_CTX(state), &states[STATE_GNSS]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_gnss_entry(void *obj)
{
	int err;
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_DBG("%s", __func__);

	err = set_gnss_active_mode(state);
	if (err) {
		LOG_ERR("Unable to set GNSS active");

		return;
	}
}

static enum smf_state_result state_gnss_run(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		switch (msg->type) {
		case NTN_LOCATION_SEARCH_DONE:
			/* Location search completed, transition to NTN mode */
			memcpy(&state->last_pvt, &msg->pvt, sizeof(state->last_pvt));
			smf_set_state(SMF_CTX(state), &states[STATE_NTN]);

			return SMF_EVENT_HANDLED;
		case GNSS_SEARCH_FAILED:
			LOG_ERR("GNSS search failed, going to idle state");
			smf_set_state(SMF_CTX(state), &states[STATE_IDLE]);

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_gnss_exit(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	set_gnss_inactive_mode();
}

static void state_ntn_entry(void *obj)
{
	int err;
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_DBG("%s", __func__);

	err = set_ntn_active_mode(state);
	if (err) {
		LOG_ERR("Failed to set NTN active mode, error: %d", err);
	}
}

static enum smf_state_result state_ntn_run(void *obj)
{
	int err;
	struct ntn_state_object *state = (struct ntn_state_object *)obj;
	char buf[NRF_CLOUD_CLIENT_ID_MAX_LEN];

	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		if (msg->type == NETWORK_CONNECTED) {
			int64_t timestamp_ms = NRF_CLOUD_NO_TIMESTAMP;
			bool confirmable = IS_ENABLED(CONFIG_APP_NTN_CLOUD_CONFIRMABLE_MESSAGES);
			struct nrf_cloud_gnss_data gnss_data = {
				.type = NRF_CLOUD_GNSS_TYPE_PVT,
				.ts_ms = timestamp_ms,
				.pvt = {
					.lat = state->last_pvt.latitude,
					.lon = state->last_pvt.longitude,
					.accuracy = state->last_pvt.accuracy,
				}
			};

			err = nrf_cloud_client_id_get(buf, sizeof(buf));
			if (err == 0) {
				LOG_INF("Connecting to nRF Cloud CoAP with client ID: %s", buf);
			} else {
				LOG_ERR("nrf_cloud_client_id_get, error: %d, cannot continue", err);
				ntn_msg_publish(SET_NTN_IDLE);

				return SMF_EVENT_PROPAGATE;
			}

			err = nrf_cloud_coap_connect(APP_VERSION_STRING);
			if (err == 0) {
				LOG_INF("nRF Cloud CoAP connection successful");
			} else if (err == -EACCES || err == -ENOEXEC || err == -ECONNREFUSED) {
				LOG_WRN("nrf_cloud_coap_connect, error: %d", err);
				LOG_WRN("nRF Cloud CoAP connection failed, unauthorized or invalid credentials");
				ntn_msg_publish(SET_NTN_IDLE);

				return SMF_EVENT_PROPAGATE;
			} else {
				LOG_WRN("nRF Cloud CoAP connection refused");
				ntn_msg_publish(SET_NTN_IDLE);

				return SMF_EVENT_PROPAGATE;
			}

			LOG_DBG("Sending to nrfcloud GNSS location data: lat: %f, lon: %f, acc: %f",
				(double)state->last_pvt.latitude,
				(double)state->last_pvt.longitude,
				(double)state->last_pvt.accuracy);

			/* Send GNSS location data to nRF Cloud */
			err = nrf_cloud_coap_location_send(&gnss_data, confirmable);
			if (err) {
				LOG_ERR("nrf_cloud_coap_location_send, error: %d", err);
			} else {
				LOG_INF("GNSS location data sent to nRF Cloud successfully");
			}

			/*
			 * We should wait until we get ACK for data being transmitted if !confirmable,
			 * and cast CFUN=45 only after data were sent.
			 * It may take 10s to send data in NTN.
			 */
			if (!confirmable) {
				k_sleep(K_MSEC(20000));
			}

			err = nrf_cloud_coap_pause();
			if ((err < 0) && (err != -EBADF)) {
				/* -EBADF means cloud was disconnected */
				LOG_ERR("Error pausing connection: %d", err);
			}

			ntn_msg_publish(SET_NTN_IDLE);
		} else if (msg->type == NETWORK_NO_SUITABLE_CELL) {
			smf_set_state(SMF_CTX(state), &states[STATE_IDLE]);
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_ntn_exit(void *obj)
{
	int err;

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	err = set_ntn_offline_mode();
	if (err) {
		LOG_ERR("Failed to set NTN offline mode, error: %d", err);
	}
}

static void ntn_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = CONFIG_APP_NTN_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC;
	const uint32_t execution_time_ms =
		(CONFIG_APP_NTN_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	struct ntn_state_object ntn_state = { 0 };

	task_wdt_id = task_wdt_add(wdt_timeout_ms, ntn_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		SEND_FATAL_ERROR();

		return;
	}

	/* Initialize state machine */
	smf_set_initial(SMF_CTX(&ntn_state), &states[STATE_RUNNING]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}

		/* Wait for messages */
		err = zbus_sub_wait_msg(&ntn_subscriber, &ntn_state.chan, ntn_state.msg_buf,
					zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}

		/* Run state machine */
		err = smf_run_state(SMF_CTX(&ntn_state));
		if (err) {
			LOG_ERR("Failed to run state machine, error: %d", err);

			continue;
		}
	}
}

K_THREAD_DEFINE(ntn_module_thread_id,
		CONFIG_APP_NTN_THREAD_STACK_SIZE,
		ntn_module_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
