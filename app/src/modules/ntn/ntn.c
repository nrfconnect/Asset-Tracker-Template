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
#include <date_time.h>
#include <modem/nrf_modem_lib.h>
#include <modem/ntn.h>
#include <nrf_modem_at.h>
#include <modem/at_monitor.h>
#include <nrf_modem_gnss.h>
#include <modem/modem_info.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/net/socket.h>
#include <errno.h>

#include "app_common.h"
#include "ntn.h"
#include "button.h"

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
ZBUS_MSG_SUBSCRIBER_DEFINE(ntn);

/* Observe NTN channel */
ZBUS_CHAN_ADD_OBS(NTN_CHAN, ntn, 0);
ZBUS_CHAN_ADD_OBS(BUTTON_CHAN, ntn, 0);

#define MAX_MSG_SIZE sizeof(struct ntn_msg)

/* State machine states */
enum ntn_module_state {
	STATE_RUNNING,
	STATE_GNSS,
	STATE_NTN,
	STATE_IDLE,
};

/* State object */
struct ntn_state_object {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	uint8_t msg_buf[MAX_MSG_SIZE];
	struct k_timer ntn_trigger_timer;
	struct k_timer network_connection_timer;
	struct k_timer rrc_connected_timer;
	struct nrf_modem_gnss_pvt_data_frame last_pvt;
	int sock_fd;
	uint64_t location_validity_end_time;
	int64_t  modem_cell_found_time;
	int64_t  modem_connectivity_time;
	int64_t  pdn_resumed_time;
	bool rrc_is_connected;
	bool is_registered;
	bool ntn_dwell_armed;
};

static struct k_work ntn_trigger_timer_work;
static struct k_work network_connection_timer_work;
static struct k_work rrc_connected_timer_work;
static struct k_work gnss_location_work;
static struct k_work gnss_timeout_work;

/* Forward declarations */

static void cereg_mon(const char *notif);

static void gnss_event_handler(int event);
static void lte_lc_evt_handler(const struct lte_lc_evt *const evt);
static void ntn_msg_publish(enum ntn_msg_type type);
static void publish_last_pvt(const struct nrf_modem_gnss_pvt_data_frame *pvt);
static void apply_gnss_time(const struct nrf_modem_gnss_pvt_data_frame *pvt_data);

static void state_running_entry(void *obj);
static enum smf_state_result state_running_run(void *obj);
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
				NULL, &states[STATE_GNSS]),
	[STATE_GNSS] = SMF_CREATE_STATE(state_gnss_entry, state_gnss_run, state_gnss_exit,
				&states[STATE_RUNNING], NULL),
	[STATE_NTN] = SMF_CREATE_STATE(state_ntn_entry, state_ntn_run, state_ntn_exit,
				&states[STATE_RUNNING], NULL),
	[STATE_IDLE] = SMF_CREATE_STATE(state_idle_entry, state_idle_run, NULL,
				&states[STATE_RUNNING], NULL),
};


/* Event handlers */

/* Timer callback for NTN mode  */
static void ntn_trigger_timer_handler(struct k_timer *timer)
{
	k_work_submit(&ntn_trigger_timer_work);
}

/* Timer callback for network connection timeout */
static void network_connection_timer_handler(struct k_timer *timer)
{
	k_work_submit(&network_connection_timer_work);
}

/* Timer callback for RRC connected dwell (post-uplink before CFUN offline) */
static void rrc_connected_timer_handler(struct k_timer *timer)
{
	k_work_submit(&rrc_connected_timer_work);
}

static void ntn_trigger_timer_work_fn(struct k_work *work)
{
	/* Time to enable NTN and connect */
	ntn_msg_publish(NTN_TRIGGER);
}

static void network_connection_timer_work_fn(struct k_work *work)
{
	/* Network connection timeout */
	LOG_WRN("Network connection timeout occurred");
	ntn_msg_publish(NTN_NETWORK_CONNECTION_TIMEOUT);
}

static void rrc_connected_timer_work_fn(struct k_work *work)
{
	/* RRC connected timeout */
	LOG_WRN("RRC connected timeout");
	ntn_msg_publish(RRC_CONNECTED_TIMEOUT);
}

static void handle_gnss_timeout_work_fn(struct k_work *work)
{
	/* GNSS timeout */
	ntn_msg_publish(GNSS_TIMEOUT);
}

static void gnss_location_work_fn(struct k_work *work)
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

static void lte_lc_evt_handler(const struct lte_lc_evt *const evt)
{
	if (evt == NULL) {
		return;
	}

	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if (evt->nw_reg_status == LTE_LC_NW_REG_UICC_FAIL) {
			/* cereg 90 */
			LOG_ERR("No SIM card detected!");
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_NOT_REGISTERED) {
			LOG_WRN("Not registered, check rejection cause");
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) {
			/* cereg 1 */
			LOG_DBG("LTE_LC_NW_REG_REGISTERED_HOME");
			ntn_msg_publish(NTN_CELL_FOUND);
			ntn_msg_publish(NTN_NETWORK_REGISTERED);
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {
			/* cereg 5 */
			LOG_DBG("LTE_LC_NW_REG_REGISTERED_ROAMING");
			ntn_msg_publish(NTN_CELL_FOUND);
			ntn_msg_publish(NTN_NETWORK_REGISTERED);
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_SEARCHING) {
			/* cereg 2 */
			LOG_DBG("LTE_LC_NW_REG_SEARCHING");
			ntn_msg_publish(NTN_CELL_FOUND);
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTRATION_DENIED) {
			/* cereg 3 */
			LOG_DBG("LTE_LC_NW_REG_REGISTRATION_DENIED");
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_NO_SUITABLE_CELL) {
			/* cereg 91 */
			LOG_DBG("LTE_LC_NW_REG_NO_SUITABLE_CELL");
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_UNKNOWN) {
			/* cereg 4 */
			LOG_DBG("LTE_LC_NW_REG_UNKNOWN");
		}

		break;

	case LTE_LC_EVT_PDN:
		switch (evt->pdn.type) {
		case LTE_LC_EVT_PDN_ACTIVATED:
			LOG_DBG("PDN connection activated");
			ntn_msg_publish(NTN_NETWORK_CONNECTED);

			break;
		case LTE_LC_EVT_PDN_DEACTIVATED:
			LOG_DBG("PDN connection deactivated");
			ntn_msg_publish(NTN_NETWORK_DISCONNECTED);

			break;
		case LTE_LC_EVT_PDN_NETWORK_DETACH:
			LOG_DBG("PDN connection network detached");
			ntn_msg_publish(NTN_NETWORK_DISCONNECTED);

			break;
		case LTE_LC_EVT_PDN_SUSPENDED:
			LOG_DBG("PDN connection suspended");
			ntn_msg_publish(NTN_NETWORK_DISCONNECTED);

			break;
		case LTE_LC_EVT_PDN_RESUMED:
			LOG_DBG("PDN connection resumed");
			ntn_msg_publish(NTN_PDN_RESUMED);

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
			ntn_msg_publish(NTN_RRC_CONNECTED);
		}
		else if (evt->rrc_mode == LTE_LC_RRC_MODE_IDLE) {
			LOG_DBG("LTE_LC_RRC_MODE_IDLE");
			ntn_msg_publish(NTN_RRC_IDLE);
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

		ntn_msg_publish(NTN_LOCATION_REQUEST);

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

/* Helper functions */

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

/*
 * lte_lc's cereg module filters out +CEREG notifications when cell ID and
 * registration status are unchanged. After PDN resume onto the same cell,
 * this means LTE_LC_EVT_NW_REG_STATUS / LTE_LC_EVT_CELL_UPDATE never fire
 * and modem_cell_found_time is never set. Monitor +CEREG directly to work
 * around this.
 *
 * Parsing is intentionally defensive:
 *  - Locate the ':' rather than assuming a fixed prefix length, so a missing
 *    space or slightly different formatting does not produce garbage.
 *  - Skip whitespace, require at least one digit, then atoi the value.
 *  - Only act on the registration-status values we care about; everything
 *    else (including unparsable input) is ignored silently.
 */
static void cereg_mon(const char *notif)
{
	enum lte_lc_nw_reg_status status;
	const char *p;

	if (notif == NULL) {
		return;
	}

	p = strchr(notif, ':');
	if (p == NULL) {
		return;
	}
	p++;

	while (*p == ' ' || *p == '\t') {
		p++;
	}

	if (*p < '0' || *p > '9') {
		return;
	}

	status = (enum lte_lc_nw_reg_status)atoi(p);

	switch (status) {
	case LTE_LC_NW_REG_REGISTERED_HOME:
	case LTE_LC_NW_REG_REGISTERED_ROAMING:
		ntn_msg_publish(NTN_CELL_FOUND);
		ntn_msg_publish(NTN_NETWORK_REGISTERED);
		break;
	case LTE_LC_NW_REG_SEARCHING:
		ntn_msg_publish(NTN_CELL_FOUND);
		break;
	default:
		break;
	}
}

AT_MONITOR(cereg_monitor, "+CEREG", cereg_mon, PAUSED);

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

#if defined(CONFIG_APP_NTN_CHANNEL_SELECT_ENABLE)
static int configure_ntn_channel_select(void)
{
	int err;

#if defined(CONFIG_APP_NTN_IRIDIUM)
	err = nrf_modem_at_printf("AT%%FREQRANGES=1,7,,1,\"%i\",\"\"",
				  CONFIG_APP_NTN_CHANNEL_SELECT);
	if (err) {
		LOG_ERR("Failed to set NTN channel using AT%%FREQRANGES, error: %d", err);
		return err;
	}
#else
	err = nrf_modem_at_printf("AT%%CHSELECT=2,14,%i", CONFIG_APP_NTN_CHANNEL_SELECT);
	if (err == 0) {
		return 0;
	}

	LOG_WRN("AT%%CHSELECT failed (%d), trying AT%%FREQRANGES fallback", err);

	err = nrf_modem_at_printf("AT%%FREQRANGES=0");
	if (err) {
		LOG_ERR("Failed to clear AT%%FREQRANGES before programming, error: %d", err);
		return err;
	}

	/*
	 * Use an "allowed" satellite NB-IoT frequency range entry so the modem
	 * searches only the configured NTN EARFCN when %CHSELECT is unavailable.
	 */
	err = nrf_modem_at_printf("AT%%FREQRANGES=1,6,,1,\"%i\",\"\"",
				  CONFIG_APP_NTN_CHANNEL_SELECT);
	if (err) {
		LOG_ERR("Failed to set NTN channel using AT%%FREQRANGES, error: %d", err);
		return err;
	}

	LOG_INF("Configured NTN channel using AT%%FREQRANGES fallback");
#endif

	return 0;
}
#endif

static int set_ntn_active_mode(struct ntn_state_object *state)
{
	int err;
	enum lte_lc_func_mode mode;
	uint32_t location_validity_time;
	uint64_t current_time = k_uptime_get();

	if (state->location_validity_end_time == 0) {
		location_validity_time = 0;  /* Infinite validity */
	}
	else if (state->location_validity_end_time > current_time) {
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

	/* Configure NTN system mode */
#if defined(CONFIG_APP_NTN_IRIDIUM)
	err = nrf_modem_at_printf("AT%%XSYSTEMMODE=0,0,0,0,0,1");
	if (err) {
		LOG_ERR("Failed to set XSYSTEMMODE=0,0,0,0,0,1, error: %d", err);

	return err;
	}
#else
	err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_NTN_NBIOT, LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	if (err) {
		LOG_ERR("Failed to set NTN system mode, error: %d", err);

		return err;
	}
#endif

	err = ntn_location_set((double)state->last_pvt.latitude,
				       (double)state->last_pvt.longitude,
				       (float)state->last_pvt.altitude,
				       location_validity_time);
	if (err) {
		LOG_ERR("Failed to set location, error: %d", err);

		return err;
	}

#if defined(CONFIG_APP_NTN_BANDLOCK_ENABLE)
	err = nrf_modem_at_printf("AT%%XBANDLOCK=2,,\"%i\"", CONFIG_APP_NTN_BANDLOCK);
	if (err) {
		LOG_ERR("Failed to set NTN band lock, error: %d", err);

		return err;
	}
#endif

#if defined(CONFIG_APP_NTN_CHANNEL_SELECT_ENABLE)
	err = configure_ntn_channel_select();
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

static int set_ntn_offline_mode(void)
{
	int err;

	/* Set modem to offline mode without loosing registration  */
	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set, error: %d", err);

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
	err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_GPS, LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	if (err) {
		LOG_ERR("Failed to set GNSS system mode, error: %d", err);

		return err;
	}

	/* Activate GNSS fun mode */
	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS);
	if (err) {
		LOG_ERR("Failed to activate GNSS fun mode, error: %d", err);

		return err;
	}

	err = nrf_modem_gnss_fix_interval_set(0);
	if (err) {
		LOG_ERR("Failed to set GNSS fix interval, error: %d", err);
	}

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

static int sock_send_dummy(struct ntn_state_object *state)
{
	int err;
	static const char dummy[] = "Dummy";

	if (state->sock_fd < 0) {
		LOG_ERR("Socket not connected");

		return -ENOTCONN;
	}

	err = send(state->sock_fd, dummy, sizeof(dummy) - 1, 0);
	if (err < 0) {
		LOG_ERR("Failed to send dummy packet, error: %d", errno);

		return -errno;
	}

	LOG_DBG("Sent dummy packet (%zu bytes)", sizeof(dummy) - 1);

	return 0;
}

static int sock_send_gnss_data(struct ntn_state_object *state)
{
	int err;
	char message[256];
	char rsrp[16] = {0}, band[16] = {0}, ue_mode[16] = {0}, oper[16] = {0}, imei[16] = {0}, temp[16] = {0};
	int32_t packet_delay;
	char imei_suffix[5];
	size_t imei_len;

	if (state->sock_fd < 0) {
		LOG_ERR("Socket not connected");

		return -ENOTCONN;
	}

	err = modem_info_string_get(MODEM_INFO_IMEI, imei, sizeof(imei));
	if (err < 0) {
		LOG_WRN("Failed to get modem IMEI, error: %d", err);
		snprintk(imei, sizeof(imei), "N/A");
	}

	err = modem_info_string_get(MODEM_INFO_RSRP, rsrp, sizeof(rsrp));
	if (err < 0) {
		LOG_WRN("Failed to get modem RSRP, error: %d", err);
		snprintk(rsrp, sizeof(rsrp), "N/A");
	}

	err = modem_info_string_get(MODEM_INFO_CUR_BAND, band, sizeof(band));
	if (err < 0) {
		LOG_WRN("Failed to get modem band, error: %d", err);
		snprintk(band, sizeof(band), "N/A");
	}

	err = modem_info_string_get(MODEM_INFO_UE_MODE, ue_mode, sizeof(ue_mode));
	if (err < 0) {
		LOG_WRN("Failed to get modem UE mode, error: %d", err);
		snprintk(ue_mode, sizeof(ue_mode), "N/A");
	}

	err = modem_info_string_get(MODEM_INFO_OPERATOR, oper, sizeof(oper));
	if (err < 0) {
		LOG_WRN("Failed to get modem operator, error: %d", err);
		snprintk(oper, sizeof(oper), "N/A");
	}

	err = modem_info_string_get(MODEM_INFO_TEMP, temp, sizeof(temp));
	if (err < 0) {
		LOG_WRN("Failed to get modem temperature, error: %d", err);
		snprintk(temp, sizeof(temp), "N/A");
	}

	if (state->modem_cell_found_time > 0 && state->modem_connectivity_time > 0) {
		packet_delay = state->modem_connectivity_time - state->modem_cell_found_time;
	} else {
		packet_delay = -1;
	}

	/* Extract last 4 characters of IMEI safely */
	imei_len = strnlen(imei, sizeof(imei));
	if (imei_len > 4) {
		err = snprintk(imei_suffix, sizeof(imei_suffix), "%s", imei + (imei_len - 4));
		if (err < 0 || err >= sizeof(imei_suffix)) {
			LOG_ERR("Failed to get IMEI suffix, error: %d", err);

			return -EINVAL;
		}
	} else {
		err = snprintk(imei_suffix, sizeof(imei_suffix), "N/A");
		if (err < 0 || err >= sizeof(imei_suffix)) {
			LOG_ERR("Failed to get IMEI suffix, error: %d", err);

			return -EINVAL;
		}

		LOG_WRN("IMEI is too short, using N/A");
	}

	imei_suffix[sizeof(imei_suffix) - 1] = '\0';

#if defined(CONFIG_APP_NTN_THINGY_ROCKS_ENDPOINT)
	// imei,ping_rtt,rsrp,band,ue_mode,oper,lat_str,lon_str,accuracy,...
	// ...battery_str,temp_str,pressure_str,humidity_str
	snprintk(message, sizeof(message),
				"%s,,%d,%s,%s,%s,%s,%.2f,%.2f,%d,%s,%s,%s,%s",
				imei,
				packet_delay,
				rsrp,
				band,
				ue_mode,
				oper,
				state->last_pvt.latitude,
				state->last_pvt.longitude,
				(int)state->last_pvt.accuracy,
				"99.99",temp,"999.99","99.99");
#elif defined(CONFIG_APP_NTN_SEND_GNSS_DATA)
	/* Format GNSS data as string */
	err = snprintk(message, sizeof(message),
		"Device: *%s, temp: %s, lat=%.2f, lon=%.2f, alt=%.2f, "
		"time=%04d-%02d-%02d %02d:%02d:%02d",
		imei_suffix, temp,
		(double)state->last_pvt.latitude, (double)state->last_pvt.longitude, (double)state->last_pvt.altitude,
		state->last_pvt.datetime.year, state->last_pvt.datetime.month, state->last_pvt.datetime.day,
		state->last_pvt.datetime.hour, state->last_pvt.datetime.minute, state->last_pvt.datetime.seconds);
	if (err < 0 || err >= sizeof(message)) {
		LOG_ERR("Failed to format GNSS data, error: %d", err);

		return -EINVAL;
	}
#else
	err = snprintk(message, sizeof(message),
		       "Device: *%s, temp: %s",
		       imei_suffix, temp);
	if (err < 0 || err >= sizeof(message)) {
		LOG_ERR("Failed to format GNSS data, error: %d", err);

		return -EINVAL;
	}
#endif

	LOG_DBG("Sending data");
	err = send(state->sock_fd, message, strlen(message), 0);
	if (err < 0) {
		LOG_ERR("Failed to send data, error: %d", errno);
		return -errno;
	}

	LOG_DBG("Sent data payload of %d bytes", strlen(message));

	return 0;
}

/* State handlers */

static void state_running_entry(void *obj)
{
	int err;
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_DBG("%s", __func__);

	k_work_init(&ntn_trigger_timer_work, ntn_trigger_timer_work_fn);
	k_work_init(&network_connection_timer_work, network_connection_timer_work_fn);
	k_work_init(&rrc_connected_timer_work, rrc_connected_timer_work_fn);
	k_work_init(&gnss_location_work, gnss_location_work_fn);
	k_work_init(&gnss_timeout_work, handle_gnss_timeout_work_fn);

	k_timer_init(&state->ntn_trigger_timer, ntn_trigger_timer_handler, NULL);
	k_timer_init(&state->network_connection_timer, network_connection_timer_handler, NULL);
	k_timer_init(&state->rrc_connected_timer, rrc_connected_timer_handler, NULL);

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("Failed to initialize the modem library, error: %d", err);

		return;
	}

	/* Register GNSS event handler */
	nrf_modem_gnss_event_handler_set(gnss_event_handler);

	/* Register LTE event handler */
	lte_lc_register_handler(lte_lc_evt_handler);

	/* Register handler for default PDP context 0. */
	err = lte_lc_pdn_default_ctx_events_enable();
	if (err) {
		LOG_ERR("lte_lc_pdn_default_ctx_events_enable, error: %d", err);

		return;
	}

	struct lte_lc_cellular_profile ntn_profile = {
		.id = 0,
		.act = LTE_LC_ACT_NTN,
		.uicc = LTE_LC_UICC_PHYSICAL,
	};

	struct lte_lc_cellular_profile tn_profile = {
		.id = 1,
		.act = LTE_LC_ACT_LTEM || LTE_LC_ACT_NBIOT,
		.uicc = LTE_LC_UICC_PHYSICAL,
	};

#if defined(CONFIG_APP_NTN_IRIDIUM)
	err = nrf_modem_at_printf("AT%%CELLULARPRFL=2,0,8,0");
		if (err) {
			LOG_ERR("Failed to set CELLULARPRFL=2,0,8,0, error: %d", err);

			return;
		}
#else
	err = lte_lc_cellular_profile_configure(&ntn_profile);
	if (err) {
		LOG_ERR("Failed to set NTN profile, error: %d", err);

		return;
	}
#endif

	err = lte_lc_cellular_profile_configure(&tn_profile);
	if (err) {
		LOG_ERR("Failed to set TN profile, error: %d", err);

		return;
	}

	ntn_register_handler(ntn_event_handler);


	k_timer_start(&state->ntn_trigger_timer, K_MINUTES(CONFIG_APP_NTN_TIMER_TIMEOUT_MINUTES), K_NO_WAIT);
}

static enum smf_state_result state_running_run(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_DBG("%s", __func__);

	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		if (msg->type == NTN_TRIGGER) {
			/* Timer expired, restart timer and transition to NTN mode */
			k_timer_start(&state->ntn_trigger_timer,
				      K_MINUTES(CONFIG_APP_NTN_TIMER_TIMEOUT_MINUTES),
				      K_NO_WAIT);
			smf_set_state(SMF_CTX(state), &states[STATE_NTN]);

			return SMF_EVENT_HANDLED;
		}

		if (msg->type == NTN_LOCATION_REQUEST) {
			uint64_t current_time = k_uptime_get();

			if (current_time < state->location_validity_end_time) {
				LOG_DBG("NTN location is still valid, skipping location request");

				return SMF_EVENT_HANDLED;
			}

			LOG_DBG("NTN location requested, location is invalid, going to GNSS mode");

			smf_set_state(SMF_CTX(state), &states[STATE_GNSS]);

			return SMF_EVENT_HANDLED;
		}
	} else if (state->chan == &BUTTON_CHAN) {
		k_timer_start(&state->ntn_trigger_timer,
			      K_MINUTES(CONFIG_APP_NTN_TIMER_TIMEOUT_MINUTES),
			      K_NO_WAIT);
		smf_set_state(SMF_CTX(state), &states[STATE_NTN]);

		return SMF_EVENT_HANDLED;
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

		switch (msg->type) {
		case NTN_LOCATION_SEARCH_DONE:
			/* Location search completed, transition to NTN mode */
			memcpy(&state->last_pvt, &msg->pvt, sizeof(state->last_pvt));

			state->location_validity_end_time =
				k_uptime_get() +
				CONFIG_APP_NTN_LOCATION_VALIDITY_TIME_SECONDS * MSEC_PER_SEC;

			smf_set_state(SMF_CTX(state), &states[STATE_NTN]);

			return SMF_EVENT_HANDLED;

		case GNSS_TIMEOUT:
			LOG_ERR("GNSS search timed out, going to idle state");
			smf_set_state(SMF_CTX(state), &states[STATE_IDLE]);

			return SMF_EVENT_HANDLED;

		case NTN_LOCATION_REQUEST:
			LOG_DBG("NTN location requested, already in GNSS mode");

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

	state->pdn_resumed_time = 0;
	state->modem_cell_found_time = 0;
	state->modem_connectivity_time = 0;
	state->is_registered = false;
	state->ntn_dwell_armed = false;

	/* lte_lc filters out +CEREG when registration status and cell ID
	* are unchanged, which is typical after PDN resume onto the
	* same cell. Resume the AT monitor to catch these directly.
	*/
	at_monitor_resume(&cereg_monitor);

	err = set_ntn_active_mode(state);
	if (err) {
		LOG_ERR("Failed to set NTN active mode, error: %d", err);
	}

	/* Start network connection timeout timer.
	 *
	 * NTN cell search and registration take significantly longer than
	 * terrestrial NB-IoT due to long propagation delays and sparse search
	 * opportunities, so this timeout must be sized generously.
	 * Once the modem reports CEREG registered, the timer is restarted with
	 * APP_NTN_REGISTERED_TIMEOUT_SECONDS so RRC/PDN setup and the actual
	 * uplink data transfer get the time they need before CFUN=45 is issued.
	 */
	k_timer_start(&state->network_connection_timer,
		      K_SECONDS(CONFIG_APP_NTN_NETWORK_CONNECTION_TIMEOUT_SECONDS),
		      K_NO_WAIT);
}

static enum smf_state_result state_ntn_run(void *obj)
{
	int err;
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_DBG("%s", __func__);

	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		switch (msg->type) {
		case NTN_PDN_RESUMED:
			state->pdn_resumed_time = k_uptime_get();

			/* Note: PDN_RESUMED fires from the modem's internal context
			 * restore that happens during CFUN=21 activation, well before
			 * any cell acquisition or network registration. It is NOT a
			 * connectivity milestone and must not advance is_registered or
			 * shorten the connection-timeout window. The dummy uplink
			 * below is what actually triggers the modem's service request
			 * once a cell becomes available.
			 */

			LOG_DBG("PDN resumed, opening socket and sending dummy");

			err = sock_open_and_connect(state);
			if (err) {
				LOG_ERR("Failed to connect socket: %d", err);

				return SMF_EVENT_HANDLED;
			}

			err = sock_send_dummy(state);
			if (err) {
				LOG_ERR("Failed to send dummy packet: %d", err);
			}

			return SMF_EVENT_HANDLED;

		case NTN_RRC_CONNECTED:
			state->rrc_is_connected = true;
			state->modem_connectivity_time = k_uptime_get();

			if (state->pdn_resumed_time > 0) {
				int32_t delta_ms;

				delta_ms = (int32_t)(k_uptime_get() - state->pdn_resumed_time);

				LOG_DBG("RRC connected %d ms after PDN resumed", delta_ms);
			}

			if (state->sock_fd < 0) {
				LOG_WRN("RRC connected but no socket open");

				return SMF_EVENT_HANDLED;
			}

			if (state->last_pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
				err = sock_send_gnss_data(state);
				if (err) {
					LOG_ERR("Failed to send GNSS data: %d", err);
				} else {
					LOG_INF("GNSS data sent on RRC connected");
				}
			} else {
				LOG_DBG("No valid GNSS data to send");
			}

			if (!state->ntn_dwell_armed) {
				state->ntn_dwell_armed = true;

				k_timer_start(&state->network_connection_timer,
					K_SECONDS(CONFIG_APP_NTN_RRC_CONNECTED_DWELL_SECONDS),
					K_NO_WAIT);
				k_timer_start(&state->rrc_connected_timer,
					K_SECONDS(CONFIG_APP_NTN_RRC_CONNECTED_DWELL_SECONDS),
					K_NO_WAIT);
			}

			return SMF_EVENT_HANDLED;

		case NTN_RRC_IDLE:
			LOG_DBG("Setting NTN RRC state to idle");

			state->rrc_is_connected = false;

			return SMF_EVENT_HANDLED;

		case NTN_CELL_FOUND:
			at_monitor_pause(&cereg_monitor);

			if (!state->rrc_is_connected) {
				LOG_DBG("Cell found");

				state->modem_cell_found_time = k_uptime_get();
			}

			return SMF_EVENT_HANDLED;

		case NTN_NETWORK_REGISTERED:
			/* Both lte_lc_evt_handler and the +CEREG AT monitor publish
			 * NTN_NETWORK_REGISTERED on registered states (CEREG=1/5), and
			 * the modem may emit several CEREG URCs during a single attempt
			 * (e.g. on TAC change). Without gating, every event would
			 * re-arm the timer with the extended timeout and could keep
			 * STATE_NTN alive indefinitely while RRC never comes up.
			 *
			 * Extend the timeout exactly once, the first time we see
			 * registration. Subsequent events are no-ops.
			 *
			 * Implements the gating rule "do not issue CFUN=45 while
			 * registered until data transfer completes or fails".
			 */
			if (state->is_registered || state->ntn_dwell_armed) {
				return SMF_EVENT_HANDLED;
			}

			state->is_registered = true;

			LOG_INF("NTN network registered, extending timeout to %d s",
				CONFIG_APP_NTN_REGISTERED_TIMEOUT_SECONDS);
			k_timer_start(&state->network_connection_timer,
				      K_SECONDS(CONFIG_APP_NTN_REGISTERED_TIMEOUT_SECONDS),
				      K_NO_WAIT);

			return SMF_EVENT_HANDLED;

		case NTN_NETWORK_CONNECTION_TIMEOUT:
			/* The timer fires from ISR context, the work item then
			 * publishes NTN_NETWORK_CONNECTION_TIMEOUT on zbus. By the
			 * time SMF processes the message, an NTN_NETWORK_REGISTERED
			 * may already have restarted the timer with the extended
			 * "registered" timeout. Detect that case and ignore the
			 * stale timeout so the freshly armed window is honoured.
			 */
			if (k_timer_remaining_get(
				    &state->network_connection_timer) > 0) {
				LOG_DBG("Stale NTN_NETWORK_CONNECTION_TIMEOUT, "
					"timer was restarted; ignoring");
				return SMF_EVENT_HANDLED;
			}

			__fallthrough;
		case RRC_CONNECTED_TIMEOUT:
			__fallthrough;
		case NTN_NETWORK_CONNECTION_FAILED:
			smf_set_state(SMF_CTX(state), &states[STATE_IDLE]);

			return SMF_EVENT_HANDLED;

		case NTN_NETWORK_CONNECTED:
			k_timer_stop(&state->network_connection_timer);

			state->modem_connectivity_time = k_uptime_get();

			/* Network is connected, set up socket */
			err = sock_open_and_connect(state);
			if (err) {
				LOG_ERR("Failed to connect socket: %d", err);

				return SMF_EVENT_HANDLED;
			} else {
				LOG_DBG("Socket setup successfully");

			}

			if (state->last_pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
				err = sock_send_gnss_data(state);
				if (err) {
					LOG_ERR("Failed to send GNSS data: %d", err);
				}
			} else {
				LOG_DBG("No valid GNSS data to send");
			}

			if (!state->ntn_dwell_armed) {
				state->ntn_dwell_armed = true;

				k_timer_start(&state->network_connection_timer,
					K_SECONDS(CONFIG_APP_NTN_RRC_CONNECTED_DWELL_SECONDS),
					K_NO_WAIT);
				k_timer_start(&state->rrc_connected_timer,
					K_SECONDS(CONFIG_APP_NTN_RRC_CONNECTED_DWELL_SECONDS),
					K_NO_WAIT);
			}

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
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_DBG("%s", __func__);

	/* Close socket if it was open */
	if (state->sock_fd >= 0) {
		close(state->sock_fd);

		state->sock_fd = -1;
	}

	k_timer_stop(&state->network_connection_timer);
	k_timer_stop(&state->rrc_connected_timer);

	at_monitor_pause(&cereg_monitor);

	err = set_ntn_offline_mode();
	if (err) {
		LOG_ERR("Failed to set NTN offline mode, error: %d", err);
	}
}


static void state_idle_entry(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_DBG("%s", __func__);

	state->rrc_is_connected = false;
}

static enum smf_state_result state_idle_run(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

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
		.rrc_is_connected = false,
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

		/* Wait for messages */
		err = zbus_sub_wait_msg(&ntn, &ntn_state.chan, ntn_state.msg_buf, zbus_wait_ms);
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
