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
#include <modem/modem_info.h>
#include <nrf_modem_at.h>
#include <nrf_modem_gnss.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/net/socket.h>
#include <errno.h>
#include <time.h>

#include "ntn.h"

#define APP_PAYLOAD_CHANNEL_BUFFER_MAX_SIZE 224

/* Socket state */
static int sock_fd = -1;
static struct sockaddr_storage host_addr;

LOG_MODULE_REGISTER(ntn, CONFIG_APP_NTN_LOG_LEVEL);

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
	struct k_timer gnss_timer;    /* Timer for GNSS update (300s before pass) */
	struct k_timer ntn_timer;     /* Timer for LTE connection (20s before pass) */
	bool socket_connected;
	bool gnss_initialized;
	bool ntn_initialized;
	bool is_first_gnss_fix;
};

static struct nrf_modem_gnss_pvt_data_frame last_pvt;
static struct k_work gnss_timer_work;
static struct k_work ntn_timer_work;
static struct k_work gnss_location_work;

/* Forward declarations */

static void gnss_event_handler(int event);
static void lte_lc_evt_handler(const struct lte_lc_evt *const evt);

/* Forward declarations */
static void state_running_entry(void *obj);
static void state_running_run(void *obj);
static void state_gnss_entry(void *obj);
static void state_gnss_run(void *obj);
static void state_gnss_exit(void *obj);
static void state_ntn_entry(void *obj);
static void state_ntn_run(void *obj);
static void state_ntn_exit(void *obj);
static void state_idle_entry(void *obj);
static void state_idle_run(void *obj);

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
	char *parse_result = strptime(time_of_pass, "%Y-%m-%d-%H:%M:%S", &pass_time);
	if (parse_result == NULL) {
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

static void gnss_timer_work_handler(struct k_work *work)
{
	/* Time to get new GNSS fix (300s before pass) */
	ntn_msg_publish(GNSS_TIMER);
}

static void ntn_timer_work_handler(struct k_work *work)
{
	/* Time to enable LTE and connect (20s before pass) */
	ntn_msg_publish(NTN_TIMER);
}

/* Timer callback for GNSS update */
static void gnss_timer_handler(struct k_timer *timer)
{
	k_work_submit(&gnss_timer_work);
}

/* Timer callback for LTE connection */
static void ntn_timer_handler(struct k_timer *timer)
{
	k_work_submit(&ntn_timer_work);
}

static void apply_gnss_time(const struct nrf_modem_gnss_pvt_data_frame *pvt_data)
{
	struct tm gnss_time = {
		.tm_year = pvt_data->datetime.year - 1900,
		.tm_mon = pvt_data->datetime.month - 1,
		.tm_mday = pvt_data->datetime.day,
		.tm_hour = pvt_data->datetime.hour,
		.tm_min = pvt_data->datetime.minute,
		.tm_sec = pvt_data->datetime.seconds,
	};

	date_time_set(&gnss_time);
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
        memcpy(&last_pvt, &pvt_data, sizeof(last_pvt));
        apply_gnss_time(&last_pvt);
        ntn_msg_publish(NTN_LOCATION_SEARCH_DONE);

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
}

static int sgp4_propagator_compute()
{
	// Placeholder
	return CONFIG_APP_NTN_TIMER_TIMEOUT_MINUTES * 60;
}

/* Helper functions */

static int set_ntn_dormant_mode(void)
{
	int err;

	/* Set modem to dormant mode without loosing ATTACH  */
	// lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE_UICC_ON)
	err = nrf_modem_at_printf("AT+CFUN=45");
	if (err) {
		LOG_ERR("Failed to set AT+CFUN=45, error: %d", err);
		return err;
	}

	return 0;
}

static int set_ntn_active_mode(struct ntn_state_object *state)
{
	int err;

	if (state->ntn_initialized)
	{
		/* Configure NTN system mode */
		// lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_NTN_NBIOT, LTE_LC_SYSTEM_MODE_PREFER_AUTO)
		err = nrf_modem_at_printf("AT%%XSYSTEMMODE=0,0,0,0,1");
		if (err) {
			LOG_ERR("Failed to set NTN system mode, error: %d", err);
			return err;
		}

		/* Configure location using latest GNSS data */
		err = nrf_modem_at_printf("AT%%LOCATION=2,\"%f\",\"%f\",\"%f\",0,0",
					(double)last_pvt.latitude,
					(double)last_pvt.longitude,
					(double)last_pvt.altitude);
		if (err) {
			LOG_ERR("Failed to set AT%%LOCATION, error: %d", err);
			return err;
		}
		LOG_DBG("NTN initialized, using AT+CFUN=21");

		err = nrf_modem_at_printf("AT+CFUN=21");
		if (err) {
			LOG_ERR("Failed to set AT+CFUN=21, error: %d", err);
			return err;
		}

	}
	else
	{
		err = nrf_modem_at_printf("AT+CFUN=0");
		if (err) {
			LOG_ERR("Failed to set AT+CFUN=0, error: %d", err);
			return err;
		}

		err = nrf_modem_at_printf("AT+CFUN=12");
		if (err) {
			LOG_ERR("Failed to set AT+CFUN=12, error: %d", err);
			return err;
		}

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

		err = nrf_modem_at_printf("AT%%XSYSTEMMODE=0,0,0,0,1");
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
					(double)last_pvt.latitude,
					(double)last_pvt.longitude,
					(double)last_pvt.altitude);
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

		state->ntn_initialized=true;

		k_sleep(K_MSEC(5000));

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

	if (state->gnss_initialized)
	{
		/* Configure GNSS system mode */
		// lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_GPS, LTE_LC_SYSTEM_MODE_PREFER_AUTO)
		err = nrf_modem_at_printf("AT%%XSYSTEMMODE=0,0,1,0,0");
		if (err) {
			LOG_ERR("Failed to set GNSS system mode, error: %d", err);
			return err;
		}

		/* Activate GNSS mode */
		// lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS)
		err = nrf_modem_at_printf("AT+CFUN=31");
		if (err) {
			LOG_ERR("Failed to activate GNSS mode, error: %d", err);
			return err;
		}
	}
	else
	{
		/* Set modem to offline mode */
		// lte_lc_func_mode_set(LTE_LC_FUNC_MODE_POWER_OFF)
		err = nrf_modem_at_printf("AT+CFUN=0");
		if (err) {
			LOG_ERR("Failed to set AT+CFUN=0, error: %d", err);
			return err;
		}

		/* Configure GNSS system mode */
		// lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_GPS, LTE_LC_SYSTEM_MODE_PREFER_AUTO)
		err = nrf_modem_at_printf("AT%%XSYSTEMMODE=0,0,1,0,0");
		if (err) {
			LOG_ERR("Failed to set GNSS system mode, error: %d", err);
			return err;
		}

		/* Activate GNSS mode */
		// lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS)
		err = nrf_modem_at_printf("AT+CFUN=31");
		if (err) {
			LOG_ERR("Failed to activate GNSS mode, error: %d", err);
			return err;
		}
		state->gnss_initialized=true;
	}

	err = nrf_modem_gnss_fix_interval_set(0);
	err = nrf_modem_gnss_fix_retry_set(180);
	err = nrf_modem_gnss_start();

	return 0;
}

static int set_gnss_inactive_mode(void)
{
	int err;

	err = nrf_modem_gnss_stop();

	/* Set modem to CFUN=30 mode when exiting GNSS state */
	// lte_lc_func_mode_set(LTE_LC_FUNC_MODE_DEACTIVATE_GNSS)
	err = nrf_modem_at_printf("AT+CFUN=30");
	if (err) {
		LOG_ERR("Failed to set modem to CFUN=30 mode, error: %d", err);
		return err;
	}
	return 0;
}

/* Socket functions */
static int sock_open_and_connect(void)
{
	int err;
	struct sockaddr_in *server4 = ((struct sockaddr_in *)&host_addr);

	server4->sin_family = AF_INET;
	server4->sin_port = htons(CONFIG_APP_NTN_SERVER_PORT);
	
	inet_pton(AF_INET, CONFIG_APP_NTN_SERVER_ADDR, &server4->sin_addr);

	/* Create UDP socket */
	sock_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock_fd < 0) {
		LOG_ERR("Failed to create UDP socket, error: %d", errno);
		return -errno;
	}

	/* Connect socket */
	err = connect(sock_fd, (struct sockaddr *)&host_addr, sizeof(struct sockaddr_in));
	if (err < 0) {
		LOG_ERR("Failed to connect socket, error: %d", errno);
		close(sock_fd);
		sock_fd = -1;
		return -errno;
	}

	return 0;
}

static int sock_send_gnss_data(const struct nrf_modem_gnss_pvt_data_frame *gnss_data)
{
	int err;
	char message[256];


	if (gnss_data == NULL) {
		LOG_ERR("Invalid GNSS data pointer");
		return -EINVAL;
	}

	if (sock_fd < 0) {
		LOG_ERR("Socket not connected");
		return -ENOTCONN;
	}


#if defined(CONFIG_APP_NTN_THINGY_ROCKS_ENDPOINT)
	char rsrp[16] = {0}, band[16] = {0}, ue_mode[16] = {0}, oper[16] = {0}, imei[16] = {0};
	err = modem_info_string_get(MODEM_INFO_IMEI, imei, sizeof(imei));
		if (err < 0) {
			snprintf(imei, sizeof(imei), "N/A");
		}
	err = modem_info_string_get(MODEM_INFO_RSRP, rsrp, sizeof(rsrp));
	if (err < 0) {
		snprintf(rsrp, sizeof(rsrp), "N/A");
	}
	err = modem_info_string_get(MODEM_INFO_CUR_BAND, band, sizeof(band));
	if (err < 0) {
		snprintf(band, sizeof(band), "N/A");
	}
	err = modem_info_string_get(MODEM_INFO_UE_MODE, ue_mode, sizeof(ue_mode));
	if (err < 0) {
		snprintf(ue_mode, sizeof(ue_mode), "N/A");
	}
	err = modem_info_string_get(MODEM_INFO_OPERATOR, oper, sizeof(oper));
	if (err < 0) {
		snprintf(oper, sizeof(oper), "N/A");
	}
	// imei,ping_rtt,rsrp,band,ue_mode,oper,lat_str,lon_str,accuracy,...
	// ...battery_str,temp_str,pressure_str,humidity_str
	snprintf(message, sizeof(message),
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
				"99.99","25.00","999.99","99.99");
#else
	// /* Custom UDP endpoint */
	snprintf(message, sizeof(message),
		"GNSS: lat=%.2f, lon=%.2f, alt=%.2f, time=%04d-%02d-%02d %02d:%02d:%02d",
		(double)gnss_data->latitude, (double)gnss_data->longitude, (double)gnss_data->altitude,
		gnss_data->datetime.year, gnss_data->datetime.month, gnss_data->datetime.day,
		gnss_data->datetime.hour, gnss_data->datetime.minute, gnss_data->datetime.seconds);
#endif

	LOG_ERR("Sending data");
	/* Send data */
	err = send(sock_fd, message, strlen(message), 0);
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

	k_work_init(&gnss_timer_work, gnss_timer_work_handler);
	k_work_init(&ntn_timer_work, ntn_timer_work_handler);
	k_work_init(&gnss_location_work, gnss_location_work_handler);
	k_timer_init(&state->gnss_timer, gnss_timer_handler, NULL);
	k_timer_init(&state->ntn_timer, ntn_timer_handler, NULL);

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("Failed to initialize the modem library, error: %d", err);
		return;
	}

	/* Register GNSS event handler */
	nrf_modem_gnss_event_handler_set(gnss_event_handler);

	/* Register LTE event handler */
	lte_lc_register_handler(lte_lc_evt_handler);
}

static void state_running_run(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;
	int err;

	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		switch (msg->type) {
		case GNSS_TIMER:
			/* GNSS timer expired, transition to GNSS state */
			LOG_INF("GNSS timer expired, transitioning to GNSS state");
			smf_set_state(SMF_CTX(state), &states[STATE_GNSS]);
			break;
		case NTN_TIMER:
			/* LTE timer expired, transition to NTN state */
			LOG_INF("NTN timer expired, transitioning to NTN state");
			smf_set_state(SMF_CTX(state), &states[STATE_NTN]);
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
}

static void state_gnss_entry(void *obj)
{
	int err;
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_DBG("%s", __func__);

	err = set_gnss_active_mode(state);
	if (err) {
		LOG_ERR("Unable to set GNSS mode");
		return;
	}
}

static void state_gnss_run(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;
	int err;

	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		if (msg->type == NTN_LOCATION_SEARCH_DONE) {
			/* Location search completed */
			if (state->is_first_gnss_fix) {
				reschedule_timers(state, CONFIG_APP_NTN_LEO_TIME_OF_PASS);
				state->is_first_gnss_fix = false;
			}

			ntn_msg_publish(NTN_SET_IDLE);
		}
	}
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
		return;
	}
}

static void state_ntn_run(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;
	int err;

	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		if (msg->type == NTN_NETWORK_CONNECTED) {
			LOG_DBG("Received NTN_NETWORK_CONNECTED, setting up socket");
			/* Network is connected, set up socket */


			/* MOMO: Now replace this with UDP endpoint world thingy rocks*/
			err = sock_open_and_connect();
			if (err) {
				LOG_ERR("Failed to connect socket, error: %d", err);
				state->socket_connected = false;
			} else {
				LOG_DBG("Socket connected successfully");
				state->socket_connected = true;
				/* Send initial GNSS data if available */
				if (last_pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
					LOG_DBG("Sending initial GNSS data");
					err = sock_send_gnss_data(&last_pvt);
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
			In future, we should wait until we get ACK for data being transmitted,
			and cast CFUN=45 only after data were sent.
			It may take 10s to send data in NTN.
			k_sleep is added as intermediate solution
			*/
			k_sleep(K_MSEC(20000));

			ntn_msg_publish(NTN_SET_IDLE);
		}
	}
}

static void state_ntn_exit(void *obj)
{
	int err;
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	/* Close socket if it was open */
	if (sock_fd >= 0) {
		close(sock_fd);
		sock_fd = -1;
		state->socket_connected = false;
	}

	err = set_ntn_dormant_mode();
	if (err) {
		return;
	}
}


static void state_idle_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

}


static void state_idle_run(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);
}


static void lte_lc_evt_handler(const struct lte_lc_evt *const evt)
{
	if (evt == NULL) {
		return;
	}

	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if (evt->nw_reg_status == LTE_LC_NW_REG_UICC_FAIL) {
			LOG_ERR("No SIM card detected!");
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_NOT_REGISTERED) {
			LOG_WRN("Not registered, check rejection cause");
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) {
			LOG_DBG("Network connectivity established to home network");
			ntn_msg_publish(NTN_NETWORK_CONNECTED);
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {
			LOG_DBG("Network connectivity established to roaming network");
			ntn_msg_publish(NTN_NETWORK_CONNECTED);
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_SEARCHING) {
			LOG_DBG("LTE_LC_NW_REG_SEARCHING");
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTRATION_DENIED) {
			LOG_DBG("LTE_LC_NW_REG_REGISTRATION_DENIED");
			ntn_msg_publish(NTN_SET_IDLE);
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_UNKNOWN) {
			LOG_DBG("LTE_LC_NW_REG_UNKNOWN");
		}
		break;
	case LTE_LC_EVT_MODEM_EVENT:
		if (evt->modem_evt == LTE_LC_MODEM_EVT_RESET_LOOP) {
			LOG_WRN("The modem has detected a reset loop!");
		} else if (evt->modem_evt == LTE_LC_MODEM_EVT_LIGHT_SEARCH_DONE) {
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

static void ntn_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = CONFIG_APP_NTN_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC;
	struct ntn_state_object ntn_state = { 0 };
	ntn_state.gnss_initialized=false;
	ntn_state.ntn_initialized=false;
	ntn_state.is_first_gnss_fix=true;

	task_wdt_id = task_wdt_add(wdt_timeout_ms, ntn_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		return;
	}

	/* Initialize state machine */
	smf_set_initial(SMF_CTX(&ntn_state), &states[STATE_RUNNING]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			return;
		}

		/* Wait for messages */
		err = zbus_sub_wait_msg(&ntn, &ntn_state.chan, ntn_state.msg_buf, K_FOREVER);
		if (err) {
			LOG_ERR("Failed to receive message, error: %d", err);
			continue;
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
