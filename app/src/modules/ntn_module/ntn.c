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
#include <nrf_modem_at.h>
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

#include "ntn.h"
#include "button.h"
#if defined(CONFIG_APP_LED)
#include "led.h"
#endif /* CONFIG_APP_LED */


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
ZBUS_CHAN_ADD_OBS(BUTTON_CHAN, ntn, 0);

#define MAX_MSG_SIZE sizeof(struct ntn_msg)

/* State machine states */
enum ntn_module_state {
	STATE_RUNNING,
	STATE_TN,
	STATE_GNSS,
	STATE_NTN,
	STATE_IDLE,
};

/* State object */
struct ntn_state_object {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	uint8_t msg_buf[MAX_MSG_SIZE];
	struct k_timer ntn_timer;
	bool ntn_initialized;
	bool gnss_initialized;
};

static struct nrf_modem_gnss_pvt_data_frame last_pvt;
static struct k_work timer_work;
static struct k_work gnss_location_work;

/* Forward declarations */

static void gnss_event_handler(int event);
static void lte_lc_evt_handler(const struct lte_lc_evt *const evt);

/* Forward declarations */
static void state_running_entry(void *obj);
static void state_running_run(void *obj);
static void state_tn_entry(void *obj);
static void state_tn_run(void *obj);
static void state_tn_exit(void *obj);
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

static void timer_work_handler(struct k_work *work)
{
	ntn_msg_publish(NTN_TIMEOUT);
}

/* Timer callback for NTN mode timeout */
static void ntn_timer_handler(struct k_timer *timer)
{
	k_work_submit(&timer_work);
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

static int sgp4_propagator_compute()
{
	// Placeholder
	return CONFIG_APP_NTN_TIMER_TIMEOUT_MINUTES * 60;
}

/* Helper functions */

#if defined(CONFIG_APP_LED)
static inline int set_led_pattern(uint8_t red, uint8_t green, uint8_t blue) {
    struct led_msg led_msg = {
        .type = LED_RGB_SET,
        .red = red,
        .green = green,
        .blue = blue,
        .duration_on_msec = 250,
        .duration_off_msec = 2000,
        .repetitions = 5,
    };

    int err = zbus_chan_pub(&LED_CHAN, &led_msg, K_SECONDS(1));
    if (err) {
        LOG_ERR("zbus_chan_pub, error: %d", err);
        return err;
    }
    return 0;
}
#endif /* CONFIG_APP_LED */

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
		LOG_DBG("NTN initialized, using AT+CFUN=21 to connect to network");
		err = nrf_modem_at_printf("AT+CFUN=21");
		if (err) {
			LOG_ERR("Failed to set AT+CFUN=21, error: %d", err);
			return err;
		}
	}
	else
	{
		/* Set modem to minimum functionality */
		// lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE)
		err = nrf_modem_at_printf("AT+CFUN=4");
		if (err) {
			LOG_ERR("Failed to set AT+CFUN=4, error: %d", err);
			return err;
		}
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

		#if defined(CONFIG_APP_NTN_DISABLE_EPCO)
		/* Set XEPCO off */
		err = nrf_modem_at_printf("AT%%XEPCO=0");
		if (err) {
			LOG_ERR("Failed to set XEPCO off, error: %d", err);
			return err;
		}
		#endif

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

		#if defined(CONFIG_APP_NTN_BANDLOCK_ENABLE)
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

		#if defined(CONFIG_APP_NTN_APN)
			err = nrf_modem_at_printf("AT+CGDCONT=0,\"ip\",\"%s\"", CONFIG_APP_NTN_APN);
			if (err) {
				LOG_ERR("Failed to set NTN APN, error: %d", err);
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
			LOG_ERR("Failed to set AT+CFUN=31, error: %d", err);
			return err;
		}
	}
	else
	{
		/* Set modem to offline mode */
		// lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE)
		err = nrf_modem_at_printf("AT+CFUN=4");
		if (err) {
			LOG_ERR("Failed to set AT+CFUN=4, error: %d", err);
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
			LOG_ERR("Failed to set AT+CFUN=31, error: %d", err);
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
		LOG_ERR("Failed to set AT+CFUN=30, error: %d", err);
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
		return;
	}

	/* Register GNSS event handler */
	nrf_modem_gnss_event_handler_set(gnss_event_handler);

	/* Register LTE event handler */
	lte_lc_register_handler(lte_lc_evt_handler);

	/* Init nrfcloud coap */
	err = nrf_cloud_coap_init();
	if (err) {
		LOG_ERR("nrf_cloud_coap_init, error: %d", err);
		return;
	}
}

static void state_running_run(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_DBG("%s", __func__);

	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;
		if (msg->type == SET_NTN_IDLE) {
			smf_set_state(SMF_CTX(state), &states[STATE_IDLE]);
		}
	} else if (state->chan == &BUTTON_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;
		if (msg->type == BUTTON_PRESS_LONG) {
			smf_set_state(SMF_CTX(state), &states[STATE_TN]);
		}
	}
}

static void state_tn_entry(void *obj)
{
	int err;

	ARG_UNUSED(obj);

	LOG_INF("Entering TN mode");

#if defined(CONFIG_APP_LED)
	/* Blink Yellow for TN network search */
	set_led_pattern(255, 255, 0);
#endif /* CONFIG_APP_LED */

	// Connect to network
	LOG_DBG("TN mode, using lte_lc_connect_async to connect to network");
	err = lte_lc_connect_async(lte_lc_evt_handler);
	if (err) {
		LOG_ERR("lte_lc_connect_async, error: %d\n", err);
		return;
	}
}

static void state_tn_run(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;
	int err;
	char buf[NRF_CLOUD_CLIENT_ID_MAX_LEN];

	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		if (msg->type == NETWORK_CONNECTED) {
			LOG_DBG("Received NETWORK_CONNECTED, connecting to nRFCloud");

		#if defined(CONFIG_APP_LED)
			/* Blue pattern for Network connected */
			set_led_pattern(0, 0, 255);
		#endif /* CONFIG_APP_LED */


			err = nrf_cloud_client_id_get(buf, sizeof(buf));
			if (err == 0) {
				LOG_INF("Connecting to nRF Cloud CoAP with client ID: %s", buf);
			} else {
				LOG_ERR("nrf_cloud_client_id_get, error: %d, cannot continue", err);
				return;
			}

			err = nrf_cloud_coap_connect(APP_VERSION_STRING);
			if (err == 0) {
				LOG_INF("nRF Cloud CoAP connection successful");
			} else if (err == -EACCES || err == -ENOEXEC || err == -ECONNREFUSED) {
				LOG_WRN("nrf_cloud_coap_connect, error: %d", err);
				LOG_WRN("nRF Cloud CoAP connection failed, unauthorized or invalid credentials");
				return;
			} else {
				LOG_WRN("nRF Cloud CoAP connection refused");
				return;
			}

			LOG_INF("CLOUD connected via TN network");

		#if defined(CONFIG_APP_LED)
			/* Green pattern for cloud connection */
			set_led_pattern(0, 255, 0);
		#endif /* CONFIG_APP_LED */

			k_sleep(K_MSEC(5000));
			err = nrf_cloud_coap_pause();
			if ((err < 0) && (err != -EBADF)) {
				/* -EBADF means cloud was disconnected */
				LOG_ERR("Error pausing connection: %d", err);
			}

			ntn_msg_publish(SET_NTN_IDLE);
		} else if (msg->type == NETWORK_LTE_OUT_OF_COVERAGE) {
			LOG_INF("Out of LTE coverage, going to idle state. Proceed to perform cloud handshake via NTN");
			ntn_msg_publish(SET_NTN_IDLE);
		}
	}
}

static void state_tn_exit(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);
}

static void state_idle_entry(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_DBG("%s", __func__);

#if defined(CONFIG_APP_LED)
	/* White pattern when idle */
	set_led_pattern(255, 255, 255);
#endif /* CONFIG_APP_LED */

// For LEO, compute new wake up using SGP4 (Simplified General Perturbations Model 4)
#if defined(CONFIG_APP_NTN_LEO)
	int sgp4_timeout_in_seconds;
	sgp4_timeout_in_seconds = sgp4_propagator_compute();
	with k_timer_start(&state->ntn_timer, K_SECONDS(sgp4_timeout_in_seconds), K_NO_WAIT);
	return;
#endif
	k_timer_start(&state->ntn_timer, K_MINUTES(CONFIG_APP_NTN_TIMER_TIMEOUT_MINUTES), K_NO_WAIT);
}

static void state_idle_run(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_DBG("%s", __func__);

	if (state->chan == &BUTTON_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;
		if (msg->type == BUTTON_PRESS_SHORT) {
			smf_set_state(SMF_CTX(state), &states[STATE_GNSS]);
		}
	} else if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;
		if (msg->type == NTN_TIMEOUT) {
			smf_set_state(SMF_CTX(state), &states[STATE_GNSS]);
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
		LOG_ERR("Unable to set GNSS active");
		return;
	}

#if defined(CONFIG_APP_LED)
	/* Purple pattern during GNNS search */
	set_led_pattern(128, 0, 128);
#endif /* CONFIG_APP_LED */
}

static void state_gnss_run(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		if (msg->type == NTN_LOCATION_SEARCH_DONE) {
			/* Location search completed, transition to NTN mode */
			smf_set_state(SMF_CTX(state), &states[STATE_NTN]);
		} else if (msg->type == GNSS_SEARCH_FAILED) {
			LOG_ERR("GNSS_SEARCH_FAILED");
			smf_set_state(SMF_CTX(state), &states[STATE_IDLE]);
		}
	}
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
		return;
	}

#if defined(CONFIG_APP_LED)
	/* Orange pattern during NTN network search */
	set_led_pattern(255, 165, 0);
#endif /* CONFIG_APP_LED */
}

static void state_ntn_run(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;
	int err;
	char buf[NRF_CLOUD_CLIENT_ID_MAX_LEN];

	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		if (msg->type == NETWORK_CONNECTED) {
			LOG_DBG("Received NETWORK_CONNECTED, connecting to nRFCloud");
		#if defined(CONFIG_APP_LED)
			/* Blue pattern for network connected */
			set_led_pattern(0, 0, 255);
		#endif /* CONFIG_APP_LED */

			err = nrf_cloud_client_id_get(buf, sizeof(buf));
			if (err == 0) {
				LOG_INF("Connecting to nRF Cloud CoAP with client ID: %s", buf);
			} else {
				LOG_ERR("nrf_cloud_client_id_get, error: %d, cannot continue", err);
				return;
			}

			err = nrf_cloud_coap_connect(APP_VERSION_STRING);
			if (err == 0) {
				LOG_INF("nRF Cloud CoAP connection successful");
			} else if (err == -EACCES || err == -ENOEXEC || err == -ECONNREFUSED) {
				LOG_WRN("nrf_cloud_coap_connect, error: %d", err);
				LOG_WRN("nRF Cloud CoAP connection failed, unauthorized or invalid credentials");
				return;
			} else {
				LOG_WRN("nRF Cloud CoAP connection refused");
				return;
			}

		#if defined(CONFIG_APP_LED)
			/* Green pattern during cloud connection */
			set_led_pattern(0, 255, 0);
		#endif /* CONFIG_APP_LED */

			int64_t timestamp_ms = NRF_CLOUD_NO_TIMESTAMP;
			bool confirmable = IS_ENABLED(CONFIG_APP_CLOUD_CONFIRMABLE_MESSAGES);
			struct nrf_cloud_gnss_data gnss_data = {
				.type = NRF_CLOUD_GNSS_TYPE_PVT,
				.ts_ms = timestamp_ms,
				.pvt = {
					.lat = last_pvt.latitude,
					.lon = last_pvt.longitude,
					.accuracy = last_pvt.accuracy,
				}
			};

			LOG_DBG("Sending to nrfcloud GNSS location data: lat: %f, lon: %f, acc: %f",
				(double)last_pvt.latitude,
				(double)last_pvt.longitude,
				(double)last_pvt.accuracy);

			/* Send GNSS location data to nRF Cloud */
			err = nrf_cloud_coap_location_send(&gnss_data, confirmable);
			if (err) {
				LOG_ERR("nrf_cloud_coap_location_send, error: %d", err);
			} else {
				LOG_INF("GNSS location data sent to nRF Cloud successfully");
			}

			/*
			We should wait until we get ACK for data being transmitted if !confirmable,
			and cast CFUN=45 only after data were sent.
			It may take 10s to send data in NTN.
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
		} else if (msg->type == NETWORK_CONNECTING_FAILED) {
			smf_set_state(SMF_CTX(state), &states[STATE_IDLE]);
		}
	}

}

static void state_ntn_exit(void *obj)
{
	int err;

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	err = set_ntn_dormant_mode();
	if (err) {
		return;
	}
}

static void lte_lc_evt_handler(const struct lte_lc_evt *const evt)
{
	LOG_DBG("Network EVT TYPE received :%d",evt->type);
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if (evt->nw_reg_status == LTE_LC_NW_REG_UICC_FAIL) {
			LOG_ERR("No SIM card detected!");
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_NOT_REGISTERED) {
			LOG_WRN("Not registered, check rejection cause");
			ntn_msg_publish(NETWORK_CONNECTING_FAILED);
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_SEARCHING) {
			LOG_DBG("LTE_LC_NW_REG_SEARCHING");
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_UNKNOWN) {
			/*
			Actually in future the proper way of handling this should probably
			 be based on CEREG=91, not CEREG=4.
			 This need to be added to lte_lc.
			*/
			LOG_WRN("LTE_LC_NW_REG_UNKNOWN");
			ntn_msg_publish(NETWORK_LTE_OUT_OF_COVERAGE);
		}else if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) {
			LOG_DBG("Network connectivity established to home network");
			ntn_msg_publish(NETWORK_CONNECTED);
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {
			LOG_DBG("Network connectivity established to roaming network");
			ntn_msg_publish(NETWORK_CONNECTED);
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
	default:
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

static void ntn_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = CONFIG_APP_NTN_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC;
	struct ntn_state_object ntn_state = { 0 };
	ntn_state.gnss_initialized=false;
	ntn_state.ntn_initialized=false;


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
