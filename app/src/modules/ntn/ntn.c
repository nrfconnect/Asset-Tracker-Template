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
#include <modem/modem_info.h>
#include <nrf_modem_at.h>
#include <modem/at_monitor.h>
#include <nrf_modem_gnss.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/net/socket.h>
#include <errno.h>
#include <time.h>

#include "app_common.h"
#include "ntn.h"

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

#define MAX_MSG_SIZE	sizeof(struct ntn_msg)

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
	struct k_timer keepalive_timer;
	struct k_timer gnss_timer;
	struct k_timer ntn_timer;
	struct k_timer idle_timer;
	bool pdn_context_active;
	bool socket_connected;
	int connection_retry_counter;
	int sock_fd;
	struct nrf_modem_gnss_pvt_data_frame last_pvt;
};

static struct k_work keepalive_timer_work;
static struct k_work gnss_timer_work;
static struct k_work ntn_timer_work;
static struct k_work idle_timer_work;

static struct k_work gnss_location_work;
static struct k_work gnss_timeout_work;

/* Forward declarations */

static void gnss_event_handler(int event);
static void lte_lc_evt_handler(const struct lte_lc_evt *const evt);
static void pdn_event_handler(uint8_t cid, enum pdn_event event, int reason);
static void ntn_msg_publish(enum ntn_msg_type type);

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

// static void log_stack_usage(const char *location)
// {
// #ifdef CONFIG_THREAD_STACK_INFO
// 	size_t unused;
// 	k_tid_t current = k_current_get();
// 	const char *thread_name = k_thread_name_get(current);
	
// 	if (k_thread_stack_space_get(current, &unused) == 0) {
// 		// Try to get stack size - this is architecture dependent
// 		size_t used = 0;
// 		size_t total = 0;
		
// 		struct k_thread *thread = (struct k_thread *)current;
// 		if (thread->stack_info.size > 0) {
// 			total = thread->stack_info.size;
// 			used = total - unused;
// 		}
		
// 		if (total > 0) {
// 			LOG_WRN("[%s] Thread: '%s' (ID:%p), Stack: %zu/%zu used, %zu free",
// 				location,
// 				thread_name ? thread_name : "unnamed",
// 				(void *)current,
// 				used, total, unused);
// 		} else {
// 			LOG_WRN("[%s] Thread: '%s' (ID:%p), Stack: %zu bytes free",
// 				location,
// 				thread_name ? thread_name : "unnamed",
// 				(void *)current,
// 				unused);
// 		}
// 	}
// #endif
// }

/* Event handlers */

static void keepalive_timer_work_handler(struct k_work *work)
{
	int err;

	// log_stack_usage("keepalive_before_publish");

	ntn_msg_publish(KEEPALIVE_TIMER);

	/* Time to send AT+CFUN? to keep USB alive */
	err = nrf_modem_at_printf("AT+CFUN?");
	if (err) {
		LOG_ERR("Failed to set AT+CFUN?, error: %d", err);

		return;
	}
}


static void gnss_timer_work_handler(struct k_work *work)
{
	/* Time to get new GNSS fix (300s before pass) */
	ntn_msg_publish(GNSS_TRIGGER);
}

static void ntn_timer_work_handler(struct k_work *work)
{
	/* Time to enable NTN and connect (20s before pass) */
	ntn_msg_publish(NTN_TRIGGER);
}

static void idle_timer_work_handler(struct k_work *work)
{
	/* Time to enable NTN and connect (20s before pass) */
	ntn_msg_publish(NTN_SET_IDLE);
}

/* Timer callback for keepalive */
static void keepalive_timer_handler(struct k_timer *timer)
{
	k_work_submit(&keepalive_timer_work);
}


/* Timer callback for GNSS update */
static void gnss_timer_handler(struct k_timer *timer)
{
	k_work_submit(&gnss_timer_work);
}

/* Timer callback for NTN connection */
static void ntn_timer_handler(struct k_timer *timer)
{
	k_work_submit(&ntn_timer_work);
}

/* Timer callback for setting idle state */
static void idle_timer_handler(struct k_timer *timer)
{
	k_work_submit(&idle_timer_work);
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
			ntn_msg_publish(SET_IDLE_TIMER);
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
			ntn_msg_publish(SET_IDLE_TIMER);
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_NO_SUITABLE_CELL) {
			/* cereg 91 */
			LOG_DBG("LTE_LC_NW_REG_NO_SUITABLE_CELL");
			// ntn_msg_publish(RESCHEDULE_NTN_TRIGGER);
			ntn_msg_publish(SET_IDLE_TIMER);
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_UNKNOWN) {
			/* cereg 4 */
			LOG_DBG("LTE_LC_NW_REG_UNKNOWN");
			// ntn_msg_publish(RESCHEDULE_NTN_TRIGGER);
			ntn_msg_publish(SET_IDLE_TIMER);
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

/* Event handlers */
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
		ntn_msg_publish(NTN_NETWORK_CONNECTED);

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

static void handle_timeout_work_fn(struct k_work *item)
{
	ARG_UNUSED(item);

	ntn_msg_publish(NTN_SET_IDLE);
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
static int reschedule_timers(struct ntn_state_object *state, const char * const time_of_pass)
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

	/* Convert to Unix timestamp */
	time_t pass_timestamp = timegm(&pass_time);

	/* Calculate time until pass */
	int64_t seconds_until_pass = pass_timestamp - current_time;
	LOG_INF("Current time: %lld, Pass time: %lld", current_time, (int64_t)pass_timestamp);
	LOG_INF("Seconds until pass: %lld", seconds_until_pass);

	if (seconds_until_pass < 0) {
		LOG_ERR("Satellite already passed");

		return -ETIME;
	}

	/* Start GNSS timer to wake up 300 seconds before pass */
	int64_t gnss_timeout_value = seconds_until_pass - CONFIG_APP_NTN_TIMER_GNSS_VALUE_SECONDS;
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

static void cereg_mon(const char *notif)
{
	enum lte_lc_nw_reg_status status = atoi(notif + (sizeof("+CEREG: ") - 1));

	if ((status == LTE_LC_NW_REG_REGISTERED_ROAMING) ||
	    (status == LTE_LC_NW_REG_REGISTERED_HOME)) {
		LOG_DBG("Network registration status: %s",
			status == LTE_LC_NW_REG_REGISTERED_ROAMING ? "ROAMING" : "HOME");
		ntn_msg_publish(NTN_NETWORK_CONNECTED);
		LOG_DBG("Stop monitoring incoming CEREG Notifications");
		at_monitor_pause(&cereg_monitor);
	}
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
	bool ntn_initialized = false;

	err = lte_lc_func_mode_get(&mode);
	if (err) {
		LOG_ERR("Failed to get LTE function mode, error: %d", err);

		return err;
	}

	/* If needed, go offline to be able to set NTN system mode */
	switch (mode) {
	case LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG:;
		ntn_initialized = true;

		break;
	case LTE_LC_FUNC_MODE_OFFLINE: __fallthrough;
	case LTE_LC_FUNC_MODE_POWER_OFF:
		break;
	default:

		break;
	}

	if (ntn_initialized) {
		/* Configure NTN system mode */
		err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_NTN_NBIOT, LTE_LC_SYSTEM_MODE_PREFER_AUTO);
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

		/* Check if we are in offline-while-keeping-registration mode. If so, the modem has already
		 * been able to register to an NTN network, which means that the LTE link controller will
		 * ignore +CEREG notifications with status 1 or 5. To work around this, we monitor the
		 * +CEREG notifications in the application.
		 */
		if (state->pdn_context_active) {
			LOG_DBG("Start monitoring incoming CEREG Notifications");
			at_monitor_resume(&cereg_monitor);
		}

		err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_LTE);
		if (err) {
			LOG_ERR("lte_lc_func_mode_set, error: %d", err);

			return err;
		}
	} else {
		/* Power off modem */
		err = lte_lc_power_off();
		if (err) {
			LOG_ERR("lte_lc_power_off, error: %d", err);

			return err;
		}

		// err = nrf_modem_at_printf("AT%%XOPCONF=23");
		// if (err) {
		// 	LOG_ERR("Failed to set AT%%XOPCONF=23, error: %d", err);

		// 	return err;
		// }

#if defined(CONFIG_APP_NTN_CELLULARPRFL_ENABLE)
		/* Set NTN profile */
		err = nrf_modem_at_printf("AT%%CELLULARPRFL=2,0,4,0");
		if (err) {
			LOG_ERR("Failed to set modem NTN profile, error: %d", err);

			return err;
		}

		/* Set TN profile */
		err = nrf_modem_at_printf("AT%%CELLULARPRFL=2,1,1,0");
		if (err) {
			LOG_ERR("Failed to set modem TN profile, error: %d", err);

			return err;
		}
#endif

		/* Configure NTN system mode */
		err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_NTN_NBIOT, LTE_LC_SYSTEM_MODE_PREFER_AUTO);
		if (err) {
			LOG_ERR("Failed to set NTN system mode, error: %d", err);

			return err;
		}

#if defined(CONFIG_APP_NTN_COPS_ENABLE)
		err = nrf_modem_at_printf("AT+COPS=1,2,\"%s\"", CONFIG_APP_NTN_COPS);
		if (err) {
			LOG_ERR("Failed to set AT+COPS=1,2,\"90197\", error: %d", err);

			return err;
		}
#endif

		err = nrf_modem_at_printf("AT+CEREG=5");
		if (err) {
			LOG_ERR("Failed to set AT+CEREG=5, error: %d", err);

			return err;
		}

#if defined(CONFIG_APP_NTN_APN)
		err = nrf_modem_at_printf("AT+CGDCONT=0,\"ip\",\"%s\"", CONFIG_APP_NTN_APN);
		if (err) {
			LOG_ERR("Failed to set NTN APN, error: %d", err);

			return err;
		}
#endif

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

		/*
		Modem is activating AT+CPSMS via CONFIG_LTE_LC_PSM_MODULE=y.
		Cast AT+CPSMS=0 to deactivate legacy PSM.
		CFUN=45 + legacy PSM is not supported, has bugs.
		*/
		err = nrf_modem_at_printf("AT+CPSMS=0");
		if (err) {
			LOG_ERR("Failed to set AT+CPSMS=0, error: %d", err);

			return err;
		}

		LOG_DBG("Pause monitoring incoming CEREG Notifications");
		at_monitor_pause(&cereg_monitor);


		configure_periodic_search();

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
	int periodic_fix_retry = 180;
	enum lte_lc_func_mode mode;

	err = lte_lc_func_mode_get(&mode);
	if (err) {
		LOG_ERR("Failed to get LTE function mode, error: %d", err);

		return err;
	}

	if ((mode != LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG)) {
		/* Go offline to be able to set GNSS system mode */
		err = lte_lc_power_off();
		if (err) {
			LOG_ERR("lte_lc_power_off, error: %d", err);

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
	char message[CONFIG_APP_NTN_SEND_1200_BYTES ? 1200 : 256];

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
		snprintk(imei, sizeof(imei), "N/A");
		if (err < 0 || err >= sizeof(imei)) {
			LOG_ERR("Failed to get IMEI, error: %d", err);

			return -EINVAL;
		}
	}
	err = modem_info_string_get(MODEM_INFO_RSRP, rsrp, sizeof(rsrp));
	if (err < 0) {
		snprintk(rsrp, sizeof(rsrp), "N/A");
		if (err < 0 || err >= sizeof(rsrp)) {
			LOG_ERR("Failed to get RSRP, error: %d", err);

			return -EINVAL;
		}
	}
	err = modem_info_string_get(MODEM_INFO_CUR_BAND, band, sizeof(band));
	if (err < 0) {
		snprintk(band, sizeof(band), "N/A");
		if (err < 0 || err >= sizeof(band)) {
			LOG_ERR("Failed to get BAND, error: %d", err);

			return -EINVAL;
		}
	}
	err = modem_info_string_get(MODEM_INFO_UE_MODE, ue_mode, sizeof(ue_mode));
	if (err < 0) {
		snprintk(ue_mode, sizeof(ue_mode), "N/A");
		if (err < 0 || err >= sizeof(ue_mode)) {
			LOG_ERR("Failed to get UE_MODE, error: %d", err);

			return -EINVAL;
		}
	}
	err = modem_info_string_get(MODEM_INFO_OPERATOR, oper, sizeof(oper));
	if (err < 0) {
		snprintk(oper, sizeof(oper), "N/A");
		if (err < 0 || err >= sizeof(oper)) {
			LOG_ERR("Failed to get OPER, error: %d", err);

			return -EINVAL;
		}
	}
	err = modem_info_string_get(MODEM_INFO_TEMP, temp, sizeof(temp));
	if (err < 0) {
		snprintk(temp, sizeof(temp), "N/A");
		if (err < 0 || err >= sizeof(temp)) {
			LOG_ERR("Failed to get TEMP, error: %d", err);

			return -EINVAL;
		}
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
#if CONFIG_APP_NTN_SEND_1200_BYTES
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


/* State handlers */

static void state_running_entry(void *obj)
{
	int err;
	struct ntn_state_object *state = (struct ntn_state_object *)obj;
	
	LOG_DBG("%s", __func__);

	k_work_init(&keepalive_timer_work, keepalive_timer_work_handler);
	k_work_init(&gnss_timer_work, gnss_timer_work_handler);
	k_work_init(&ntn_timer_work, ntn_timer_work_handler);
	k_work_init(&idle_timer_work, idle_timer_work_handler);

	k_timer_init(&state->keepalive_timer, keepalive_timer_handler, NULL);
	k_timer_init(&state->gnss_timer, gnss_timer_handler, NULL);
	k_timer_init(&state->ntn_timer, ntn_timer_handler, NULL);
	k_timer_init(&state->idle_timer, idle_timer_handler, NULL);

	k_work_init(&gnss_location_work, gnss_location_work_handler);
	k_work_init(&gnss_timeout_work, handle_timeout_work_fn);

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
	err = pdn_default_ctx_cb_reg(pdn_event_handler);
	if (err) {
		LOG_ERR("pdn_default_ctx_cb_reg, error: %d", err);

		return;
	}

	k_work_submit(&keepalive_timer_work);
}

static enum smf_state_result state_running_run(void *obj)
{
	int err;
	struct ntn_state_object *state = (struct ntn_state_object *)obj;


	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		switch (msg->type) {
		case KEEPALIVE_TIMER:
			LOG_DBG("USB keepalive");
			k_timer_start(&state->keepalive_timer,
					K_SECONDS(300),
					K_NO_WAIT);

			break;
		case GNSS_TRIGGER:
			LOG_DBG("GNSS trigger");
			smf_set_state(SMF_CTX(state), &states[STATE_GNSS]);

			break;
		case NTN_TRIGGER:
			LOG_DBG("NTN trigger");
			if (state->connection_retry_counter < 5) {
				state->connection_retry_counter++;
				smf_set_state(SMF_CTX(state), &states[STATE_NTN]);
			} else {
				smf_set_state(SMF_CTX(state), &states[STATE_IDLE]);
			}

			break;
		case NTN_SET_IDLE:
			smf_set_state(SMF_CTX(state), &states[STATE_IDLE]);

			break;
		case NTN_SHELL_SET_TIME:
			reschedule_timers(state, msg->time_of_pass);

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
		state->socket_connected = false;
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

	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		if (msg->type == NTN_LOCATION_SEARCH_DONE) {
			/* Location search completed, transition to NTN mode */
			memcpy(&state->last_pvt, &msg->pvt, sizeof(state->last_pvt));

			ntn_msg_publish(NTN_SET_IDLE);
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_gnss_exit(void *obj)
{
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
		LOG_ERR("Failed to set ntn active mode");
	}
}

static enum smf_state_result state_ntn_run(void *obj)
{
	int err;
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		switch (msg->type) {
		case NTN_RRC_CONNECTED:
			k_timer_stop(&state->ntn_timer);

			err = nrf_modem_at_printf("AT%%XMONITOR");
			if (err) {
				LOG_ERR("Failed to set AT%%XMONITOR, error: %d", err);

				return err;
			}

			return SMF_EVENT_HANDLED;

			break;
		case RESCHEDULE_NTN_TRIGGER:
			k_timer_start(&state->ntn_timer,
				K_SECONDS(15),
				K_NO_WAIT);

			return SMF_EVENT_HANDLED;

			break;
		case SET_IDLE_TIMER:
			k_timer_start(&state->idle_timer,
				K_SECONDS(180),
				K_NO_WAIT);


			return SMF_EVENT_HANDLED;

			break;
		case NTN_NETWORK_CONNECTED:
			if (!state->pdn_context_active) {
				state->pdn_context_active = true;
			}

			LOG_DBG("Setting up socket");

			/* Network is connected, set up socket */
			err = sock_open_and_connect(state);
			if (err) {
				LOG_ERR("Failed to connect socket, error: %d", err);

				state->socket_connected = false;
			} else {
				LOG_DBG("Socket connected successfully");

				state->socket_connected = true;

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

			ntn_msg_publish(NTN_SET_IDLE);

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

	/* Close socket if it was open */
	if (state->sock_fd >= 0) {
		close(state->sock_fd);
		state->sock_fd = -1;
		state->socket_connected = false;
	}

	k_timer_stop(&state->ntn_timer);

	err = set_ntn_offline_mode();
	if (err) {
		LOG_ERR("Failed to set ntn dormant mode");
	}
}


static void state_idle_entry(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_DBG("%s", __func__);

	state->connection_retry_counter = 0;
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
		.connection_retry_counter = 0,
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
