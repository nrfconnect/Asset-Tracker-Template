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
#include <memfault/components.h>
#include <memfault/ports/zephyr/http.h>
#include <memfault/metrics/metrics.h>
#include <memfault/core/data_packetizer.h>
#include <memfault/core/trace_event.h>
#include "memfault/panics/coredump.h"
#include "memfault_lte_coredump_modem_trace.h"
#include <modem/nrf_modem_lib_trace.h>
#include <date_time.h>
#include <modem/nrf_modem_lib.h>
#include <modem/ntn.h>
#include <modem/modem_info.h>
#include <nrf_modem_at.h>
#include <nrf_modem_gnss.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/net/socket.h>
#include <errno.h>
#include <time.h>

#include <net/nrf_cloud.h>
#include <net/nrf_cloud_coap.h>
#include <net/nrf_cloud_rest.h>
#include <nrf_cloud_coap_transport.h>
#include <app_version.h>


#include "cbor_helper.h"

#include "app_common.h"
#include "ntn.h"
#if defined(CONFIG_TLE_VIA_HTTP)
#include "celestrak_client.h"
#endif
#include "sat_prediction.h"

LOG_MODULE_REGISTER(ntn_module, CONFIG_APP_NTN_LOG_LEVEL);

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

#define MAX_MSG_SIZE	sizeof(struct ntn_msg)

/* State machine states */
enum ntn_module_state {
	STATE_RUNNING,
	STATE_TN,
	STATE_GNSS,
	STATE_SGP4,
	STATE_NTN,
	STATE_IDLE,
};

/* State object */
struct ntn_state_object {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	uint8_t msg_buf[MAX_MSG_SIZE];
	struct k_timer keepalive_timer;
	struct k_timer ntn_timer;
	struct k_timer gnss_timer;
	struct k_timer network_connection_timeout_timer;
	int sock_fd;
	struct nrf_modem_gnss_pvt_data_frame last_pvt;
	/* TLE storage */
	char tle_name[30];
	char tle_line1[80];
	char tle_line2[80];
	bool has_valid_tle;
	bool has_valid_gnss;
	uint64_t location_validity_end_time;
	bool run_sgp4_after_gnss;
};

static struct k_work keepalive_timer_work;
static struct k_work ntn_timer_work;
static struct k_work gnss_timer_work;
static struct k_work network_connection_timeout_work;

static struct k_work gnss_location_work;
static struct k_work gnss_timeout_work;

/* Forward declarations */

static void gnss_event_handler(int event);
static void lte_lc_evt_handler(const struct lte_lc_evt *const evt);
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
static void state_sgp4_entry(void *obj);
static enum smf_state_result state_sgp4_run(void *obj);
static void state_sgp4_exit(void *obj);
static void state_idle_entry(void *obj);
static enum smf_state_result state_idle_run(void *obj);

/* State machine definition */
static const struct smf_state states[] = {
	[STATE_RUNNING] = SMF_CREATE_STATE(state_running_entry, state_running_run, NULL,
				NULL, &states[STATE_TN]),
	[STATE_GNSS] = SMF_CREATE_STATE(state_gnss_entry, state_gnss_run, state_gnss_exit,
				&states[STATE_RUNNING], NULL),
	[STATE_TN] = SMF_CREATE_STATE(state_tn_entry, state_tn_run, state_tn_exit,
				&states[STATE_RUNNING], NULL),
	[STATE_SGP4] = SMF_CREATE_STATE(state_sgp4_entry, state_sgp4_run, state_sgp4_exit,
				&states[STATE_RUNNING], NULL),
	[STATE_NTN] = SMF_CREATE_STATE(state_ntn_entry, state_ntn_run, state_ntn_exit,
				&states[STATE_RUNNING], NULL),
	[STATE_IDLE] = SMF_CREATE_STATE(state_idle_entry, state_idle_run, NULL,
				&states[STATE_RUNNING], NULL),
};


/* Event handlers */

static void keepalive_timer_work_fn(struct k_work *work)
{
	int err;

	LOG_INF("USB keepalive, needed for Windows setup");

	/* Time to send AT+CFUN? to keep USB alive */
	err = nrf_modem_at_printf("AT+CFUN?");
	if (err) {
		LOG_ERR("Failed to set AT+CFUN?, error: %d", err);

		return;
	}

	ntn_msg_publish(KEEPALIVE_TIMER);
}

static void ntn_timer_work_fn(struct k_work *work)
{
	/* Time to enable NTN and connect */
	ntn_msg_publish(NTN_TRIGGER);
}

static void handle_gnss_timeout_work_fn(struct k_work *work)
{
	/* GNSS timeout */
	ntn_msg_publish(GNSS_TIMEOUT);
}

static void network_connection_timeout_work_fn(struct k_work *work)
{
	/* Network connection timeout */
	LOG_WRN("Network connection timeout occurred");
	ntn_msg_publish(NETWORK_CONNECTION_TIMEOUT);
}

/* Timer callback for keepalive */
static void keepalive_timer_handler(struct k_timer *timer)
{
	k_work_submit(&keepalive_timer_work);
}

/* Timer callback for NTN connection */
static void ntn_timer_handler(struct k_timer *timer)
{
	k_work_submit(&ntn_timer_work);
}

/* Timer callback for network connection timeout */
static void network_connection_timeout_handler(struct k_timer *timer)
{
	k_work_submit(&network_connection_timeout_work);
}

/* Timer callback for GNSS fix */
static void gnss_timer_handler(struct k_timer *timer)
{
	k_work_submit(&gnss_timer_work);
}

static void gnss_timer_work_fn(struct k_work *work)
{
	/* Time to get GNSS fix */
	ntn_msg_publish(GNSS_TRIGGER);
}

static void lte_lc_evt_handler(const struct lte_lc_evt *const evt)
{
	if (evt == NULL) {
		return;
	}

	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if (evt->nw_reg_status == LTE_LC_NW_REG_UICC_FAIL) {
			/* cereg 90*/
			LOG_ERR("No SIM card detected!");
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_NOT_REGISTERED) {
			/* cereg 0 */
			LOG_DBG("LTE_LC_NW_REG_NOT_REGISTERED");
			LOG_WRN("Not registered, check rejection cause");
			ntn_msg_publish(NETWORK_CONNECTION_FAILED);
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) {
			/* cereg 1 */
			LOG_DBG("LTE_LC_NW_REG_REGISTERED_HOME");
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {
			/* cereg 5 */
			LOG_DBG("LTE_LC_NW_REG_REGISTERED_ROAMING");
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_SEARCHING) {
			/* cereg 2 */
			LOG_DBG("LTE_LC_NW_REG_SEARCHING");
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTRATION_DENIED) {
			/* cereg 3 */
			LOG_DBG("LTE_LC_NW_REG_REGISTRATION_DENIED");
			ntn_msg_publish(NETWORK_CONNECTION_FAILED);
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_NO_SUITABLE_CELL) {
			/* cereg 91 */
			LOG_DBG("LTE_LC_NW_REG_NO_SUITABLE_CELL");
			ntn_msg_publish(NETWORK_CONNECTION_FAILED);
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_UNKNOWN) {
			/* cereg 4 */
			LOG_DBG("LTE_LC_NW_REG_UNKNOWN");
		}

		break;
	case LTE_LC_EVT_PDN:
		switch (evt->pdn.type) {
		case LTE_LC_EVT_PDN_ACTIVATED:
			LOG_DBG("PDN connection activated");
			ntn_msg_publish(NETWORK_CONNECTED);

			break;
		case LTE_LC_EVT_PDN_DEACTIVATED:
			LOG_DBG("PDN connection deactivated");
			ntn_msg_publish(NETWORK_DISCONNECTED);

			break;
		case LTE_LC_EVT_PDN_NETWORK_DETACH:
			LOG_DBG("PDN connection network detached");
			ntn_msg_publish(NETWORK_DISCONNECTED);

			break;
		case LTE_LC_EVT_PDN_SUSPENDED:
			LOG_DBG("PDN connection suspended");
			ntn_msg_publish(NETWORK_DISCONNECTED);

			break;
		case LTE_LC_EVT_PDN_RESUMED:
			LOG_DBG("PDN connection resumed");
			ntn_msg_publish(NETWORK_CONNECTED);

			break;
		default:
			break;
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
	default:
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

		ntn_msg_publish(LOCATION_REQUEST);

		break;
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

static void ntn_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("NTN watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));
}

/* Helper function to publish NTN messages */
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

/* Helper functions */

static void configure_periodic_search(void) {
	struct lte_lc_periodic_search_cfg search_cfg = { 0 };

	search_cfg.pattern_count = 1;
	search_cfg.loop = true;
	search_cfg.return_to_pattern = 0;
	search_cfg.band_optimization = 0;

	search_cfg.patterns[0].type = LTE_LC_PERIODIC_SEARCH_PATTERN_TABLE;
	search_cfg.patterns[0].table.val_1 = 2;
	search_cfg.patterns[0].table.val_2 = -1;
	search_cfg.patterns[0].table.val_3 = -1;
	search_cfg.patterns[0].table.val_4 = -1;
	search_cfg.patterns[0].table.val_5 = -1;

	lte_lc_periodic_search_set(&search_cfg);

	return;
}


static int parse_time_of_pass(const char *time_str, struct tm *out)
{
    if (sscanf(time_str, "%d-%d-%d-%d:%d:%d",
               &out->tm_year, &out->tm_mon, &out->tm_mday,
               &out->tm_hour, &out->tm_min, &out->tm_sec) != 6) {
        return -EINVAL;
    }

    out->tm_year -= 1900;
    out->tm_mon  -= 1;
    return 0;
}

/* Helper function to parse time and set up timers */
static int reschedule_next_pass(struct ntn_state_object *state, const char * const time_of_pass)
{
	int err;
	int64_t current_time;
	
	/* Get current time */
	err = date_time_now(&current_time);
	if (err) {
		LOG_ERR("Failed to get current time: %d", err);

		return err;
	}

	current_time = current_time / 1000;

	/* Parse configured time of pass */
	struct tm pass_time = {0};
	if (parse_time_of_pass(time_of_pass, &pass_time) < 0) {
		LOG_ERR("Failed to parse configured time of pass");
		return -EINVAL;
	}

	/* Convert to Unix timestamp using date_time API */
	int64_t pass_timestamp;
	struct tm *utc_time = &pass_time;
	err = date_time_set(utc_time);
	if (err) {
		LOG_ERR("Failed to set date time: %d", err);
		return err;
	}
	err = date_time_now(&pass_timestamp);
	if (err) {
		LOG_ERR("Failed to get timestamp: %d", err);
		return err;
	}
	pass_timestamp = pass_timestamp / 1000; /* Convert from ms to seconds */

	/* Calculate time until pass */
	int64_t seconds_until_pass = pass_timestamp - current_time;
	LOG_INF("Current time: %lld, Pass time: %lld", current_time, (int64_t)pass_timestamp);
	LOG_INF("Seconds until pass: %lld", seconds_until_pass);

	if (seconds_until_pass < 0) {
		LOG_ERR("Satellite already passed");

		return -ETIME;
	}


	/* Start GNSS timer to wake up 5 minutes before pass */
	int64_t gnss_timeout_value = seconds_until_pass - (5 * 60);
	k_timer_start(&state->gnss_timer,
			K_SECONDS(gnss_timeout_value),
			K_NO_WAIT);

	/* Start LTE timer to wake up 20 seconds before pass */
	int64_t ntn_timeout_value = seconds_until_pass - CONFIG_APP_NTN_TIMER_NTN_VALUE_SECONDS;
	k_timer_start(&state->ntn_timer,
			K_SECONDS(ntn_timeout_value),
			K_NO_WAIT);

	LOG_INF("GNSS timer set to wake up in %lld seconds", gnss_timeout_value);
	LOG_INF("NTN timer set to wake up in %lld seconds", ntn_timeout_value);

	return 0;
}

static int set_ntn_offline_mode(void)
{
	int err;

	/* Set modem to dormant mode without losing registration  */
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
	uint32_t location_validity_time;
	uint64_t current_time = k_uptime_get();

	if (state->location_validity_end_time > current_time) {
		location_validity_time =
			(uint32_t)(state->location_validity_end_time - current_time) / MSEC_PER_SEC;
	} else {
		location_validity_time = 1;
	}

	err = lte_lc_func_mode_get(&mode);
	if (err) {
		LOG_ERR("Failed to get LTE function mode, error: %d", err);

		return err;
	}

	/* If needed, go offline to be able to set NTN system mode */
	switch (mode) {
	case LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG: __fallthrough;
	case LTE_LC_FUNC_MODE_OFFLINE: __fallthrough;
	case LTE_LC_FUNC_MODE_POWER_OFF:
		break;
	default:
		err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE);
		if (err) {
			LOG_ERR("lte_lc_func_mode_set, error: %d", err);

			return err;
		}

		break;
	}

	/* Select physical SIM for NTN mode */
	err = nrf_modem_at_printf("AT%%CSUS=0");
	if (err) {
		LOG_ERR("Failed to select physical SIM, error: %d", err);
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

#if defined(CONFIG_APP_NTN_BANDLOCK_ENABLE)
	err = nrf_modem_at_printf("AT%%XBANDLOCK=1,,\"%i\"", CONFIG_APP_NTN_BANDLOCK);
	if (err) {
		LOG_ERR("Failed to set NTN band lock, error: %d", err);

		return err;
	}
#endif

#if defined(CONFIG_APP_NTN_CHANNEL_SELECT_ENABLE)
	err = nrf_modem_at_printf("AT%%CHSELECT=2,14,%i", CONFIG_APP_NTN_CHANNEL_SELECT);
	if (err) {
		LOG_ERR("Failed to set NTN channel, error: %d", err);

		return err;
	}
#endif

	configure_periodic_search();

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_LTE);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set, error: %d\n", err);

		return err;
	}

	return 0;
}

static int set_gnss_active_mode(struct ntn_state_object *state)
{
	int err;
	int periodic_fix_retry = 180;
	enum lte_lc_func_mode mode;

	err = lte_lc_func_mode_get(&mode);
	if (err) {
		LOG_ERR("Failed to get LTE function mode, error: %d", err);

		return err;
	}

	if ((mode != LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG)) {
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
	err = nrf_modem_gnss_fix_retry_set(periodic_fix_retry);
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

/* Socket functions */
static int sock_open_and_connect(struct ntn_state_object *state)
{
	int err;
	struct sockaddr_storage host_addr;
	struct sockaddr_in *server4 = ((struct sockaddr_in *)&host_addr);

	server4->sin_family = AF_INET;
	server4->sin_port = htons(CONFIG_APP_NTN_SERVER_PORT);
	
	(void)inet_pton(AF_INET, CONFIG_APP_NTN_SERVER_ADDR, &server4->sin_addr);

	/* Create UDP socket */
	state->sock_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (state->sock_fd < 0) {
		LOG_ERR("Failed to create UDP socket, error: %d", errno);

		return -errno;
	}

	/* Connect socket */
	err = connect(state->sock_fd, (struct sockaddr *)&host_addr, sizeof(struct sockaddr_in));
	if (err < 0) {
		LOG_ERR("Failed to connect socket, error: %d", errno);
		close(state->sock_fd);
		state->sock_fd = -1;

		return -errno;
	}

	return 0;
}

static int sock_send_gnss_data(struct ntn_state_object *state)
{
	int err;
#if defined(CONFIG_APP_NTN_SEND_1200_BYTES)
	char message[1200];
#else
	char message[256];
#endif
	const struct nrf_modem_gnss_pvt_data_frame *gnss_data = &state->last_pvt;


	if (state->sock_fd < 0) {
		LOG_ERR("Socket not connected");

		return -ENOTCONN;
	}

#if defined(CONFIG_APP_NTN_THINGY_ROCKS_ENDPOINT)
	char rsrp[16] = {0}, band[16] = {0}, ue_mode[16] = {0}, oper[16] = {0}, imei[16] = {0};
	char temp[16] = {0};
	err = modem_info_string_get(MODEM_INFO_IMEI, imei, sizeof(imei));
	if (err < 0) {
		LOG_WRN("Failed to get modem IMEI, error: %d. Using fallback value.", err);
		snprintk(imei, sizeof(imei), "000000000000000");
	}
	err = modem_info_string_get(MODEM_INFO_RSRP, rsrp, sizeof(rsrp));
	if (err < 0) {
		LOG_WRN("Failed to get modem RSRP, error: %d. Using fallback value.", err);
		snprintk(rsrp, sizeof(rsrp), "-115");
	}
	err = modem_info_string_get(MODEM_INFO_CUR_BAND, band, sizeof(band));
	if (err < 0) {
		LOG_WRN("Failed to get modem band, error: %d. Using fallback value.", err);
		snprintk(band, sizeof(band), "256");
	}
	err = modem_info_string_get(MODEM_INFO_UE_MODE, ue_mode, sizeof(ue_mode));
	if (err < 0) {
		LOG_WRN("Failed to get modem UE mode, error: %d. Using fallback value.", err);
		snprintk(ue_mode, sizeof(ue_mode), "0");
	}
	err = modem_info_string_get(MODEM_INFO_OPERATOR, oper, sizeof(oper));
	if (err < 0) {
		LOG_WRN("Failed to get modem operator, error: %d. Using fallback value.", err);
		snprintk(oper, sizeof(oper), "90197");
	}
	err = modem_info_string_get(MODEM_INFO_TEMP, temp, sizeof(temp));
	if (err < 0) {
		LOG_WRN("Failed to get modem temperature, error: %d. Using fallback value.", err);
		snprintk(temp, sizeof(temp), "20");
	}
	// imei,ping_rtt,rsrp,band,ue_mode,oper,lat_str,lon_str,accuracy,...
	// ...battery_str,temp_str,pressure_str,humidity_str
	snprintk(message, sizeof(message),
				"%s,,%d,%s,%s,%s,%s,%.2f,%.2f,%d,%s,%s,%s,%s",
				imei,
				999,
				rsrp,
				band,
				ue_mode,
				oper,
				gnss_data->latitude,
				gnss_data->longitude,
				(int)gnss_data->accuracy,
				"99.99",temp,"999.99","99.99");
#else
	// /* Custom UDP endpoint */
#if defined(CONFIG_APP_NTN_SEND_1200_BYTES)
	// Fill the message with repeating pattern to create 1200 bytes
	char base_msg[100];
	err = snprintk(base_msg, sizeof(base_msg),
		"GNSS: lat=%.2f, lon=%.2f, alt=%.2f, time=%04d-%02d-%02d %02d:%02d:%02d",
		(double)gnss_data->latitude, (double)gnss_data->longitude, (double)gnss_data->altitude,
		gnss_data->datetime.year, gnss_data->datetime.month, gnss_data->datetime.day,
		gnss_data->datetime.hour, gnss_data->datetime.minute, gnss_data->datetime.seconds);
	
	if (err < 0 || err >= sizeof(base_msg)) {
		LOG_ERR("Failed to format base GNSS string, error: %d", err);
		return -EINVAL;
	}

	// Fill the 1200 byte buffer with repeating base message
	int pos = 0;
	int base_len = strlen(base_msg);
	// Fill complete base_msg copies
	while (pos + base_len <= sizeof(message) - 2) {  // Leave room for \0
		memcpy(message + pos, base_msg, base_len);
		pos += base_len;
	}
	// Fill any remaining space with partial base_msg
	if (pos < sizeof(message) - 1) {
		int remaining = sizeof(message) - pos - 1;
		memcpy(message + pos, base_msg, remaining);
		pos += remaining;
	}
	message[pos] = '\0';
#else
	err = snprintk(message, sizeof(message),
		"GNSS: lat=%.2f, lon=%.2f, alt=%.2f, time=%04d-%02d-%02d %02d:%02d:%02d",
		(double)gnss_data->latitude, (double)gnss_data->longitude, (double)gnss_data->altitude,
		gnss_data->datetime.year, gnss_data->datetime.month, gnss_data->datetime.day,
		gnss_data->datetime.hour, gnss_data->datetime.minute, gnss_data->datetime.seconds);
	if (err < 0 || err >= sizeof(message)) {
		LOG_ERR("Failed to format GNSS string, error: %d", err);
		return -EINVAL;
	}
#endif
#endif

	LOG_DBG("Sending data");
	/* Send data */
	err = send(state->sock_fd, message, strlen(message), 0);
	if (err < 0) {
		LOG_ERR("Failed to send GNSS data, error: %d", errno);

		return -errno;
	}

	LOG_DBG("Sent GNSS data payload of %d bytes", strlen(message));

	return 0;
}


static int connect_to_cloud(void)
{
	int err;
	char buf[NRF_CLOUD_CLIENT_ID_MAX_LEN];

	/* First, check if the cloud connection is already established and we can resume it */
	err = nrf_cloud_coap_resume();
	if (err) {
		LOG_DBG("nrf_cloud_coap_resume, error: %d", err);
	} else {
		LOG_INF("Cloud connection resumed");

		return 0;
	}

	err = nrf_cloud_client_id_get(buf, sizeof(buf));
	if (err) {
		LOG_ERR("nrf_cloud_client_id_get, error: %d, cannot continue", err);

		return err;
	}

	LOG_INF("Connecting to nRF Cloud CoAP using client ID: %s", buf);

	err = nrf_cloud_coap_connect(APP_VERSION_STRING);
	if (err == -EACCES || err == -ENOEXEC || err == -ECONNREFUSED) {
		LOG_WRN("nrf_cloud_coap_connect, error: %d", err);
		LOG_WRN("nRF Cloud CoAP connection failed, unauthorized or invalid credentials");

		return err;
	} else if (err < 0) {
		LOG_WRN("nRF Cloud CoAP connection refused");

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

	k_work_init(&keepalive_timer_work, keepalive_timer_work_fn);
	k_work_init(&ntn_timer_work, ntn_timer_work_fn);
	k_work_init(&gnss_timer_work, gnss_timer_work_fn);
	k_work_init(&network_connection_timeout_work, network_connection_timeout_work_fn);

	k_timer_init(&state->keepalive_timer, keepalive_timer_handler, NULL);
	k_timer_init(&state->ntn_timer, ntn_timer_handler, NULL);
	k_timer_init(&state->gnss_timer, gnss_timer_handler, NULL);
	k_timer_init(&state->network_connection_timeout_timer, network_connection_timeout_handler, NULL);

	k_work_init(&gnss_location_work, gnss_location_work_handler);
	k_work_init(&gnss_timeout_work, handle_gnss_timeout_work_fn);

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("Failed to initialize the modem library, error: %d", err);

		return;
	}

	/* Initialize satellite prediction module */
	sat_prediction_init();

#if defined(CONFIG_TLE_VIA_HTTP)
	/* Initialize Celestrak client */
	err = celestrak_client_init();
	if (err) {
		LOG_ERR("Failed to initialize Celestrak client, error: %d", err);
		return;
	}
#endif

	/* Register GNSS event handler */
	nrf_modem_gnss_event_handler_set(gnss_event_handler);

	/* Register LTE event handler */
	lte_lc_register_handler(lte_lc_evt_handler);

	/* Register handler for default PDP context. */
	err = lte_lc_pdn_default_ctx_events_enable();
	if (err) {
		LOG_ERR("lte_lc_pdn_default_ctx_events_enable, error: %d", err);

		return;
	}

	ntn_register_handler(ntn_event_handler);

	k_work_submit(&keepalive_timer_work);

	err = lte_lc_power_off();
		if (err) {
			LOG_ERR("lte_lc_power_off, error: %d", err);

			return;
		}

	/* Set NTN SIM profile.
	 * 2: Configure cellular profile
	 * 1: Cellular profile index
	 * 4: Access technology: Satellite E-UTRAN (NB-S1 mode)
	 * 0: SIM slot, physical SIM
	 */
	struct lte_lc_cellular_profile ntn_profile = {
			.id = 1,
			.act = LTE_LC_ACT_NTN,
			.uicc = LTE_LC_UICC_PHYSICAL,
		};

	/* Set NTN profile */
	err = lte_lc_cellular_profile_configure(&ntn_profile);
		if (err) {
			LOG_ERR("Failed to set NTN profile, error: %d", err);

			return;
		}


	/* Set TN SIM profile for LTE-M
		* 2: Configure cellular profile
		* 0: Cellular profile index
		* 1: Access technology: LE-UTRAN (WB-S1 mode), LTE-M
		* 0: SIM slot, physical SIM
	*/
	struct lte_lc_cellular_profile tn_profile = {
			.id = 0,
			.act = LTE_LC_ACT_LTEM | LTE_LC_ACT_NBIOT,
			.uicc = LTE_LC_UICC_SOFTSIM,
		};

	/* Set TN profile */
	err = lte_lc_cellular_profile_configure(&tn_profile);
		if (err) {
			LOG_ERR("Failed to set TN profile, error: %d", err);

			return;
		}

	/* Init nrfcloud coap */
	err = nrf_cloud_coap_init();
	if (err) {
		LOG_ERR("nrf_cloud_coap_init, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	/* Stop modem trace collection */
	err = nrf_modem_lib_trace_level_set(NRF_MODEM_LIB_TRACE_LEVEL_OFF);
	if (err) {
		LOG_ERR("Failed to disable modem trace level: %d", err);
	}


	/* Clear any existing traces before starting collection */
	err = nrf_modem_lib_trace_clear();
	if (err) {
		LOG_ERR("Failed to clear modem trace data: %d", err);
	}
}

static enum smf_state_result state_running_run(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;


	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		switch (msg->type) {
		case KEEPALIVE_TIMER:
			k_timer_start(&state->keepalive_timer,
					K_SECONDS(300),
					K_NO_WAIT);

			break;
		case GNSS_TRIGGER:
			smf_set_state(SMF_CTX(state), &states[STATE_GNSS]);

			break;
		case NTN_TRIGGER:
			smf_set_state(SMF_CTX(state), &states[STATE_NTN]);

			break;
		case GNSS_TIMEOUT:
			smf_set_state(SMF_CTX(state), &states[STATE_IDLE]);

			break;
		case NTN_SHELL_SET_TIME:
			reschedule_next_pass(state, msg->time_of_pass);

			break;
		default:
			/* Don't care */
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}


static void state_gnss_entry(void *obj)
{
	int err;
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_DBG("%s", __func__);

	/* Close socket if it was open */
	if (state->sock_fd >= 0) {
		close(state->sock_fd);

		state->sock_fd = -1;
	}

	err = set_gnss_active_mode(state);
	if (err) {
		LOG_ERR("Unable to set GNSS mode");

		return;
	}
}

static enum smf_state_result state_gnss_run(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_DBG("%s", __func__);

	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		if (msg->type == LOCATION_SEARCH_DONE) {
			/* Store GNSS data */
			memcpy(&state->last_pvt, &msg->pvt, sizeof(state->last_pvt));
			state->has_valid_gnss = true;

			state->location_validity_end_time =
				k_uptime_get() +
				CONFIG_APP_NTN_LOCATION_VALIDITY_TIME_SECONDS * MSEC_PER_SEC;

			/* Transition based on state flag */
			if (state->run_sgp4_after_gnss) {
				smf_set_state(SMF_CTX(state), &states[STATE_SGP4]);
			} else {
				smf_set_state(SMF_CTX(state), &states[STATE_IDLE]);
			}
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_gnss_exit(void *obj)
{
	LOG_DBG("%s", __func__);

	set_gnss_inactive_mode();
}

static void state_tn_entry(void *obj)
{
	int err;
	enum lte_lc_func_mode mode;
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	/* Initialize memfault modem trace */
	err = memfault_lte_coredump_modem_trace_init();
	if (err && err != -EALREADY) {
		LOG_ERR("Failed to initialize memfault modem trace: %d", err);
		return;
	}

	err = lte_lc_func_mode_get(&mode);
	if (err) {
		LOG_ERR("Failed to get LTE function mode, error: %d", err);

		return;
	}

	/* If needed, go offline to be able to set TN system mode */
	switch (mode) {
	case LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG: __fallthrough;
	case LTE_LC_FUNC_MODE_OFFLINE: __fallthrough;
	case LTE_LC_FUNC_MODE_POWER_OFF:
		break;
	default:
		err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG);
		if (err) {
			LOG_ERR("lte_lc_func_mode_set, error: %d", err);

			return;
		}

		break;
	}

	/* Select softsim for terrestrial mode */
	err = nrf_modem_at_printf("AT%%CSUS=2");
	if (err) {
		LOG_ERR("Failed to select softsim, error: %d", err);
		return;
	}

	err = nrf_modem_at_printf("AT%%XBANDLOCK=0");
	if (err) {
		LOG_ERR("Failed to set remove NTN band lock, error: %d", err);

		return;
	}

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

	/* Connect to network */
	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_LTE);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set, error: %d", err);

		return;
	}
}


static enum smf_state_result state_tn_run(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	if (state->chan == &NTN_CHAN) {
		int err;
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		if (msg->type == NETWORK_CONNECTION_FAILED) {
			LOG_INF("Out of LTE coverage, going to idle state");
			smf_set_state(SMF_CTX(state), &states[STATE_IDLE]);

			return SMF_EVENT_HANDLED;
		} else if (msg->type == NETWORK_CONNECTED) {

#if defined(CONFIG_TLE_VIA_HTTP)
			/* Fetch TLE for SIOT1 with proper error handling */
			char tle_buffer[1024] = {0};
			size_t bytes_written = 0;
			const char* siot1_catnr = "60550";  // SATELIOT_1

			LOG_INF("Starting TLE fetch for SIOT1 via SoftSIM");

			err = celestrak_fetch_tle(siot1_catnr, tle_buffer, sizeof(tle_buffer), &bytes_written);
				
			if (err != 0 || bytes_written == 0) {
				LOG_ERR("Failed to fetch TLEs after all retries");
				return SMF_EVENT_HANDLED;
			}

			LOG_INF("TLE for SIOT1:\n%s", tle_buffer);

			/* Parse the TLE data - it comes in 3 lines format */
			char *saveptr;
			char *line = strtok_r(tle_buffer, "\n", &saveptr);
			if (!line) {
				LOG_ERR("Failed to parse satellite name");
				return SMF_EVENT_HANDLED;
			}

			/* Store TLE data */
			strncpy(state->tle_name, line, sizeof(state->tle_name) - 1);
			state->tle_name[sizeof(state->tle_name) - 1] = '\0';

			line = strtok_r(NULL, "\n", &saveptr);
			if (!line) {
				LOG_ERR("Failed to parse TLE line 1");
				return SMF_EVENT_HANDLED;
			}
			strncpy(state->tle_line1, line, sizeof(state->tle_line1) - 1);
			state->tle_line1[sizeof(state->tle_line1) - 1] = '\0';

			line = strtok_r(NULL, "\n", &saveptr);
			if (!line) {
				LOG_ERR("Failed to parse TLE line 2");
				return SMF_EVENT_HANDLED;
			}
			strncpy(state->tle_line2, line, sizeof(state->tle_line2) - 1);
			state->tle_line2[sizeof(state->tle_line2) - 1] = '\0';

			state->has_valid_tle = true;
			LOG_INF("TLE data stored successfully");
#else /* TLE via nRFCloud */
			err = connect_to_cloud();
			if (err) {
				LOG_WRN("Failed to connect to nRF Cloud CoAP on TN");
				LOG_WRN("Cloud connection is not available for resumption on NTN");

			}

			LOG_INF("Cloud connection established via TN network");

			/* Fetch TLE from shadow */
			uint8_t shadow_buf[1024];
			size_t shadow_len = sizeof(shadow_buf);

			err = nrf_cloud_coap_shadow_get(shadow_buf, &shadow_len, false, COAP_CONTENT_FORMAT_APP_CBOR);
			if (err) {
				LOG_ERR("Failed to get shadow data, error: %d", err);
			}

			/* Parse TLE from shadow */
			struct tle_data tle;
			err = decode_tle_from_shadow(shadow_buf, shadow_len, &tle);
			if (err) {
				LOG_ERR("Failed to obtain TLE data, error: %d", err);
			} else {
				/* Store TLE data */
				strncpy(state->tle_name, tle.name, sizeof(state->tle_name) - 1);
				strncpy(state->tle_line1, tle.line1, sizeof(state->tle_line1) - 1);
				strncpy(state->tle_line2, tle.line2, sizeof(state->tle_line2) - 1);

				/* Ensure null termination */
				state->tle_name[sizeof(state->tle_name) - 1] = '\0';
				state->tle_line1[sizeof(state->tle_line1) - 1] = '\0';
				state->tle_line2[sizeof(state->tle_line2) - 1] = '\0';

				state->has_valid_tle = true;
				LOG_INF("TLE data stored successfully:");
				LOG_INF("Name:  %s", state->tle_name);
				LOG_INF("Line1: %s", state->tle_line1);
				LOG_INF("Line2: %s", state->tle_line2);
			}


			/* Pause the CoAP connection to save the DTLS CID and resume it
			 * when transitioning to NTN mode.
			 */
			err = nrf_cloud_coap_pause();
			if ((err < 0) && (err != -EBADF)) {
				/* -EBADF means cloud was disconnected */
				LOG_ERR("Error pausing connection: %d", err);
			} else if (err == 0) {
				LOG_INF("CoAP connection paused");
			}
#endif


			/* Prepare modem traces for upload */
			err = memfault_lte_coredump_modem_trace_prepare_for_upload();
			if (err == -ENODATA) {
				LOG_DBG("No modem traces to upload");
			} else if (err) {
				LOG_ERR("Failed to prepare modem traces for upload: %d", err);
			} else {
				LOG_INF("Modem traces ready for upload");
				
				/* Only post data if we have traces to upload */
				err = memfault_zephyr_port_post_data();
				if (err) {
					LOG_ERR("Failed to post data to Memfault: %d", err);
				} else {
					LOG_INF("Successfully posted modem trace data to Memfault");
				}
			}

			smf_set_state(SMF_CTX(state), &states[STATE_GNSS]);

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

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set, error: %d", err);

		return;
	}
}

static void state_sgp4_entry(void *obj)
{
	int err;
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_DBG("%s", __func__);


	if (!state->has_valid_tle || !state->has_valid_gnss) {
		LOG_ERR("Missing required data for SGP4 calculation");
		smf_set_state(SMF_CTX(state), &states[STATE_IDLE]);
		return;
	}

	LOG_INF("Using SGP4 to compute next pass");

	sat_prediction_pass_t next_pass;
	err = sat_prediction_get_next_pass_with_tle(
		(double)state->last_pvt.latitude,
		(double)state->last_pvt.longitude,
		(double)state->last_pvt.altitude,
		state->tle_name,
		state->tle_line1,
		state->tle_line2,
		&next_pass
	);

	if (err) {
		LOG_ERR("Failed to get next satellite pass, error: %d", err);
		smf_set_state(SMF_CTX(state), &states[STATE_IDLE]);
		return;
	}

	/* Format time for reschedule_next_pass */
	time_t start_time = next_pass.start_time_ms / 1000;
	time_t end_time = next_pass.end_time_ms / 1000;
	struct tm start_tm, end_tm;
	char start_time_str[32], end_time_str[32];

	gmtime_r(&start_time, &start_tm);
	strftime(start_time_str, sizeof(start_time_str), "%Y-%m-%d %H:%M:%S UTC", &start_tm);
	gmtime_r(&end_time, &end_tm);
	strftime(end_time_str, sizeof(end_time_str), "%Y-%m-%d %H:%M:%S UTC", &end_tm);

	/* Convert max elevation time to readable format */
	time_t max_el_time = next_pass.max_elevation_time_ms / 1000;
	struct tm max_el_tm;
	char max_el_time_str[32];
	gmtime_r(&max_el_time, &max_el_tm);
	strftime(max_el_time_str, sizeof(max_el_time_str), "%Y-%m-%d %H:%M:%S UTC", &max_el_tm);

	LOG_INF("Next pass: %s", next_pass.sat_name);
	LOG_INF("Start: %s", start_time_str);
	LOG_INF("End: %s", end_time_str);
	LOG_INF("Max elevation: %.2f degrees at %s", next_pass.max_elevation, max_el_time_str);

	char time_str[32];
	snprintk(time_str, sizeof(time_str), "%04d-%02d-%02d-%02d:%02d:%02d",
		max_el_tm.tm_year + 1900,
		max_el_tm.tm_mon + 1,
		max_el_tm.tm_mday,
		max_el_tm.tm_hour,
		max_el_tm.tm_min,
		max_el_tm.tm_sec);

	/* Schedule timers for next pass */
	err = reschedule_next_pass(state, time_str);
	if (err) {
		LOG_ERR("Failed to reschedule timers, error: %d", err);
	}

	smf_set_state(SMF_CTX(state), &states[STATE_IDLE]);

}

static enum smf_state_result state_sgp4_run(void *obj)
{
	LOG_DBG("%s", __func__);
	
	return SMF_EVENT_PROPAGATE;
}

static void state_sgp4_exit(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_DBG("%s", __func__);

	/* Set flag to not run SGP4 after next GNSS fix */
	state->run_sgp4_after_gnss = false;
}

static void state_ntn_entry(void *obj)
{
	int err;
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_DBG("%s", __func__);

	/* Logs are automatically captured by Memfault through the Zephyr logging system */
	/* Clear any existing traces before starting collection */
	err = nrf_modem_lib_trace_clear();
	if (err) {
		LOG_ERR("Failed to clear modem trace data: %d", err);
	}

	k_sleep(K_SECONDS(1));

	/* Enable modem trace collection */
	err = nrf_modem_lib_trace_level_set(NRF_MODEM_LIB_TRACE_LEVEL_FULL);
	if (err) {
		LOG_ERR("Failed to set modem trace level: %d", err);
	}

	k_sleep(K_SECONDS(1));

	err = set_ntn_active_mode(state);
	if (err) {
		LOG_ERR("Failed to set ntn active mode");
	}

	/* Start network connection timeout timer - 3 minutes */
	k_timer_start(&state->network_connection_timeout_timer, K_MINUTES(3), K_NO_WAIT);
}

static enum smf_state_result state_ntn_run(void *obj)
{
	int err;
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_DBG("%s", __func__);

	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		switch (msg->type) {
		case NETWORK_CONNECTION_FAILED:
		case NETWORK_CONNECTION_TIMEOUT:
			smf_set_state(SMF_CTX(state), &states[STATE_TN]);
			return SMF_EVENT_HANDLED;

		case NETWORK_CONNECTED:
			/* Stop the connection timeout timer since we're connected */
			k_timer_stop(&state->network_connection_timeout_timer);
			LOG_DBG("Setting up socket");

			/* Network is connected, set up socket */
			err = sock_open_and_connect(state);
			if (err) {
				LOG_ERR("Failed to connect socket, error: %d", err);

			} else {
				LOG_DBG("Socket connected successfully");

				/* Send initial GNSS data if available */
				if (state->last_pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
					LOG_DBG("Sending initial GNSS data");

					err = sock_send_gnss_data(state);
					if (err) {
						LOG_ERR("Failed to send initial GNSS data, error: %d", err);
					} else {
						LOG_DBG("Initial GNSS data sent successfully");
					}
				} else {
					LOG_DBG("No valid GNSS data available to send initially");
				}
			}

			/*
			* In future, we should wait until we get ACK for data being transmitted,
			* and send CFUN=45 only after data were sent successfully.
			*
			* It may take 10s to send data in NTN.
			* k_sleep is added as intermediate solution
			*/
			k_sleep(K_MSEC(20000));

			smf_set_state(SMF_CTX(state), &states[STATE_TN]);

			return SMF_EVENT_HANDLED;

			break;
		default:
			/* Don't care */
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_ntn_exit(void *obj)
{
	int err;
	struct ntn_state_object *state = (struct ntn_state_object *)obj;


	k_sleep(K_SECONDS(2));

	/* Trigger Memfault log collection */
	memfault_log_trigger_collection();

	LOG_DBG("%s", __func__);

	/* Close socket if it was open */
	if (state->sock_fd >= 0) {
		close(state->sock_fd);
		state->sock_fd = -1;
	}

	k_timer_stop(&state->ntn_timer);
	k_timer_stop(&state->network_connection_timeout_timer);

	err = set_ntn_offline_mode();
	if (err) {
		LOG_ERR("Failed to set ntn dormant mode");
	}

	/* Stop modem trace collection */
	err = nrf_modem_lib_trace_level_set(NRF_MODEM_LIB_TRACE_LEVEL_OFF);
	if (err) {
		LOG_ERR("Failed to disable modem trace level: %d", err);
	}

	/* Set flag to run SGP4 after next GNSS fix */
	state->run_sgp4_after_gnss = true;


	// ntn_msg_publish(RUN_SGP4);
}

static void state_idle_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);
}


static enum smf_state_result state_idle_run(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_DBG("%s", __func__);

	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		if (msg->type == LOCATION_REQUEST) {
			uint64_t current_time = k_uptime_get();
			if (current_time < state->location_validity_end_time) {
			LOG_DBG("NTN location is still valid, skipping location request");

				return SMF_EVENT_HANDLED;
			}

			LOG_DBG("NTN location requested, location is invalid, fetching fresh TLE and GNSS");

			LOG_WRN("Skipping modem location request for now");
			// smf_set_state(SMF_CTX(state), &states[STATE_GNSS]);
		} else if (msg->type == RUN_SGP4){
			smf_set_state(SMF_CTX(state), &states[STATE_SGP4]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void ntn_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = CONFIG_APP_NTN_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC;
	const uint32_t execution_time_ms =
		(CONFIG_APP_NTN_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	struct ntn_state_object ntn_state = {
		.sock_fd = -1,
		.has_valid_tle = false,
		.has_valid_gnss = false,
		.run_sgp4_after_gnss = true,
	 };

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

	// log_stack_usage("main_loop_top");

		/* Wait for messages */
		err = zbus_sub_wait_msg(&ntn_subscriber, &ntn_state.chan, ntn_state.msg_buf, zbus_wait_ms);
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
