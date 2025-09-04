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
		 struct location_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

#define MAX_MSG_SIZE sizeof(struct location_msg)

/* Define listener for this module */
ZBUS_MSG_SUBSCRIBER_DEFINE(location);

/* Observe channels */
ZBUS_CHAN_ADD_OBS(LOCATION_CHAN, location, 0);

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

static void location_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void cloud_request_send(const struct location_data_cloud *cloud_request)
{
	int err;
	struct location_msg location_msg = {
		.type = LOCATION_CLOUD_REQUEST,
		.cloud_request = *cloud_request
	};

	err = zbus_chan_pub(&LOCATION_CHAN, &location_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

#if defined(CONFIG_NRF_CLOUD_AGNSS)
static void agnss_request_send(const struct nrf_modem_gnss_agnss_data_frame *agnss_request)
{
	int err;
	struct location_msg location_msg = {
		.type = LOCATION_AGNSS_REQUEST,
		.agnss_request = *agnss_request
	};

	err = zbus_chan_pub(&LOCATION_CHAN, &location_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}
#endif /* defined(CONFIG_NRF_CLOUD_AGNSS) */

static void gnss_location_send(const struct location_data *location_data)
{
	int err;
	struct location_msg location_msg = {
		.type = LOCATION_GNSS_DATA,
		.gnss_data = *location_data
	};

	err = zbus_chan_pub(&LOCATION_CHAN, &location_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

static void send_location_status_and_set_functional_mode(enum location_msg_type status)
{
	int err;
	struct location_msg location_msg = {
		.type = status
	};

	if ((status == LOCATION_SEARCH_DONE) && IS_ENABLED(CONFIG_LOCATION_METHOD_GNSS)) {
		err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_DEACTIVATE_GNSS);
		if (err) {
			LOG_ERR("Activating GNSS in the modem failed: %d", err);
			SEND_FATAL_ERROR();
		}
	}

	err = zbus_chan_pub(&LOCATION_CHAN, &location_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

void trigger_location_update(void)
{
	int err;

	if (IS_ENABLED(CONFIG_LOCATION_METHOD_GNSS)) {
		err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS);
		if (err) {
			LOG_ERR("Activating GNSS in the modem failed: %d", err);
			SEND_FATAL_ERROR();
		}
	}

	err = location_request(NULL);
	if (err == -EBUSY) {
		LOG_WRN("Location request already in progress");
	} else if (err) {
		LOG_ERR("Unable to send location request: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void handle_location_chan(const struct location_msg *location_msg)
{
	int err;

	if (location_msg->type == LOCATION_SEARCH_TRIGGER) {
		LOG_DBG("Location search trigger received, getting location");
		trigger_location_update();
	} else if (location_msg->type == LOCATION_SEARCH_CANCEL) {
		LOG_DBG("Location search cancel received, cancelling location request");

		err = location_request_cancel();
		if (err == -EPERM) {
			LOG_WRN("Location library not initialized, cannot cancel request");
			SEND_FATAL_ERROR();
		} else if (err) {
			LOG_ERR("Unable to cancel location request: %d", err);
		} else {
			LOG_DBG("Location request cancelled successfully");
		}
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
		handle_location_chan(MSG_TO_LOCATION_MSG_PTR(state_object->msg_buf));
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
#if defined(CONFIG_LOCATION_METHOD_GNSS)
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
#endif /* CONFIG_LOCATION_METHOD_GNSS */

static void location_event_handler(const struct location_event_data *event_data)
{
	switch (event_data->id) {
	case LOCATION_EVT_LOCATION:
		LOG_DBG("Got location: lat: %f, lon: %f, acc: %f, method: %s",
			(double) event_data->location.latitude,
			(double) event_data->location.longitude,
			(double) event_data->location.accuracy,
			location_method_str(event_data->method));

#if defined(CONFIG_LOCATION_METHOD_GNSS)
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

			/* Send GNSS location data to cloud for reporting */
			gnss_location_send(&event_data->location);
		}
#endif /* CONFIG_LOCATION_METHOD_GNSS */

		send_location_status_and_set_functional_mode(LOCATION_SEARCH_DONE);
		break;
	case LOCATION_EVT_STARTED:
		send_location_status_and_set_functional_mode(LOCATION_SEARCH_STARTED);
		break;
	case LOCATION_EVT_TIMEOUT:
		LOG_DBG("Getting location timed out");
		send_location_status_and_set_functional_mode(LOCATION_SEARCH_DONE);
		break;
	case LOCATION_EVT_ERROR:
		LOG_WRN("Location request failed:");
		LOG_WRN("Used method: %s (%d)", location_method_str(event_data->method),
								    event_data->method);

		location_print_data_details(event_data->method, &event_data->error.details);

		send_location_status_and_set_functional_mode(LOCATION_SEARCH_DONE);
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
	case LOCATION_EVT_CLOUD_LOCATION_EXT_REQUEST:
		LOG_DBG("Cloud location request received from location library");
		cloud_request_send(&event_data->cloud_location_request);
		break;
#if defined(CONFIG_NRF_CLOUD_AGNSS)
	case LOCATION_EVT_GNSS_ASSISTANCE_REQUEST:
		LOG_DBG("A-GNSS assistance request received from location library");
		agnss_request_send(&event_data->agnss_request);
		break;
#endif
	case LOCATION_EVT_RESULT_UNKNOWN:
		LOG_DBG("Location result unknown");
		send_location_status_and_set_functional_mode(LOCATION_SEARCH_DONE);
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
