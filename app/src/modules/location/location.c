/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/init.h>
#include <zephyr/smf.h>
#include <modem/location.h>
#include <nrf_modem_gnss.h>
#include <date_time.h>
#include <net/nrf_cloud.h>
#include <modem/nrf_modem_lib.h>

#include "app_common.h"
#include "modem/lte_lc.h"
#include "location.h"

LOG_MODULE_REGISTER(location_module, CONFIG_APP_LOCATION_LOG_LEVEL);

BUILD_ASSERT(CONFIG_APP_LOCATION_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_LOCATION_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

/* Define channels provided by this module */
ZBUS_CHAN_DEFINE(LOCATION_CHAN,
		 enum location_msg_type,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Define listener for this module */
ZBUS_MSG_SUBSCRIBER_DEFINE(location);

/* Observe channels */
ZBUS_CHAN_ADD_OBS(LOCATION_CHAN, location, 0);

#define MAX_MSG_SIZE sizeof(enum location_msg_type)

/* Forward declarations */
static void location_event_handler(const struct location_event_data *event_data);

/* State machine */

/* Location module states.
 */
enum location_module_state {
	/* The module is running */
	STATE_RUNNING,
};

/* State object.
 * Used to transfer context data between state changes.
 */
struct location_state_object {
	/* This must be first */
	struct smf_ctx ctx;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;

	/* Last received message */
	uint8_t msg_buf[MAX_MSG_SIZE];
};

/* Forward declarations of state handlers */
static void state_running_entry(void *obj);
static void state_running_run(void *obj);

/* State machine definition */
/* States are defined in the state object */
static const struct smf_state states[] = {
	[STATE_RUNNING] =
		SMF_CREATE_STATE(state_running_entry,
				 state_running_run,
				 NULL,
				 NULL,
				 NULL),
};

static void on_cfun(int mode, void *ctx)
{
	ARG_UNUSED(ctx);

	int err;
	static bool inited;

	/* Don't activate GNSS if modem is powered off or put into offline mode. */
	if (mode == 0 || mode == 4) {
		/* Set init guard to false if modem is disabled or turned off.
		 * This makes it possible to reactivate GNSS when modem is turned on.
		 */
		inited = false;
		return;
	}

	/* Use initialization guard to avoid an infinite loop. */
	if (inited) {
		return;
	}

	inited = true;

	LOG_DBG("Modem CFUN mode: %d", mode);

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS);
	if (err) {
		LOG_ERR("Activating GNSS in the modem failed: %d", err);
		SEND_FATAL_ERROR();
	}
}

NRF_MODEM_LIB_ON_CFUN(att_location_init_hook, on_cfun, NULL);

static void location_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void status_send(enum location_msg_type status)
{
	int err;
	enum location_msg_type location_status = status;

	err = zbus_chan_pub(&LOCATION_CHAN, &location_status, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

void trigger_location_update(void)
{
	int err;

	err = location_request(NULL);
	if (err == -EBUSY) {
		LOG_WRN("Location request already in progress");
	} else if (err) {
		LOG_ERR("Unable to send location request: %d", err);
		SEND_FATAL_ERROR();
	}
}

void handle_location_chan(enum location_msg_type location_msg_type)
{
	if (location_msg_type == LOCATION_SEARCH_TRIGGER) {
		LOG_DBG("Location search trigger received, getting location");
		trigger_location_update();
	}
}

/* State handlers */
static void state_running_entry(void *obj)
{
	ARG_UNUSED(obj);

	int err;

	LOG_DBG("%s", __func__);

	err = location_init(location_event_handler);
	if (err) {
		LOG_ERR("Unable to init location library: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	LOG_DBG("Location library initialized");
}

static void state_running_run(void *obj)
{
	struct location_state_object const *state_object = obj;

	if (state_object->chan == &LOCATION_CHAN) {
		handle_location_chan(MSG_TO_LOCATION_TYPE(state_object->msg_buf));
	}
}

static void location_print_data_details(enum location_method method,
					const struct location_data_details *details)
{
	LOG_DBG("Elapsed method time: %d ms", details->elapsed_time_method);
#if defined(CONFIG_LOCATION_METHOD_GNSS)
	if (method == LOCATION_METHOD_GNSS) {
		LOG_DBG("Satellites tracked: %d", details->gnss.satellites_tracked);
		LOG_DBG("Satellites used: %d", details->gnss.satellites_used);
		LOG_DBG("Elapsed GNSS time: %d ms", details->gnss.elapsed_time_gnss);
		LOG_DBG("GNSS execution time: %d ms", details->gnss.pvt_data.execution_time);
	}
#endif
#if defined(CONFIG_LOCATION_METHOD_CELLULAR)
	if (method == LOCATION_METHOD_CELLULAR || method == LOCATION_METHOD_WIFI_CELLULAR) {
		LOG_DBG("Neighbor cells: %d", details->cellular.ncells_count);
		LOG_DBG("GCI cells: %d", details->cellular.gci_cells_count);
	}
#endif
#if defined(CONFIG_LOCATION_METHOD_WIFI)
	if (method == LOCATION_METHOD_WIFI || method == LOCATION_METHOD_WIFI_CELLULAR) {
		LOG_DBG("Wi-Fi APs: %d", details->wifi.ap_count);
	}
#endif
}

/* Take time from PVT data and apply it to system time. */
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

static void location_event_handler(const struct location_event_data *event_data)
{
	switch (event_data->id) {
	case LOCATION_EVT_LOCATION:
		LOG_DBG("Got location: lat: %f, lon: %f, acc: %f, method: %s",
			(double) event_data->location.latitude,
			(double) event_data->location.longitude,
			(double) event_data->location.accuracy,
			location_method_str(event_data->method));

		if (event_data->method == LOCATION_METHOD_GNSS) {
			struct nrf_modem_gnss_pvt_data_frame pvt_data =
				event_data->location.details.gnss.pvt_data;
			if (event_data->location.datetime.valid) {
				/* GNSS is the most accurate time source -  use it. */
				apply_gnss_time(&pvt_data);
			} else {
				/* this should not happen */
				LOG_WRN("Got GNSS location without valid time data");
			}
		}

		status_send(LOCATION_SEARCH_DONE);
		break;
	case LOCATION_EVT_STARTED:
		status_send(LOCATION_SEARCH_STARTED);
		break;
	case LOCATION_EVT_TIMEOUT:
		LOG_DBG("Getting location timed out");
		status_send(LOCATION_SEARCH_DONE);
		break;
	case LOCATION_EVT_ERROR:
		LOG_WRN("Location request failed:");
		LOG_WRN("Used method: %s (%d)", location_method_str(event_data->method),
								    event_data->method);

		location_print_data_details(event_data->method, &event_data->error.details);

		status_send(LOCATION_SEARCH_DONE);
		break;
	case LOCATION_EVT_FALLBACK:
		LOG_DBG("Location request fallback has occurred:");
		LOG_DBG("Failed method: %s (%d)", location_method_str(event_data->method),
								      event_data->method);
		LOG_DBG("New method: %s (%d)", location_method_str(
							event_data->fallback.next_method),
							event_data->fallback.next_method);
		LOG_DBG("Cause: %s",
			(event_data->fallback.cause == LOCATION_EVT_TIMEOUT) ? "timeout" :
			(event_data->fallback.cause == LOCATION_EVT_ERROR) ? "error" :
			"unknown");

		location_print_data_details(event_data->method, &event_data->fallback.details);
		break;
	default:
		LOG_DBG("Getting location: Unknown event %d", event_data->id);
		break;
	}
}

static void location_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms =
		(CONFIG_APP_LOCATION_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_LOCATION_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	struct location_state_object location_state = { 0 };

	LOG_DBG("Location module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, location_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		SEND_FATAL_ERROR();
		return;
	}

	/* Initialize the state machine */
	smf_set_initial(SMF_CTX(&location_state), &states[STATE_RUNNING]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("Failed to feed the watchdog: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = zbus_sub_wait_msg(&location, &location_state.chan,
				       location_state.msg_buf, zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = smf_run_state(SMF_CTX(&location_state));
		if (err) {
			LOG_ERR("smf_run_state(), error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}
	}
}

K_THREAD_DEFINE(location_module_thread_id, CONFIG_APP_LOCATION_THREAD_STACK_SIZE,
		location_module_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
