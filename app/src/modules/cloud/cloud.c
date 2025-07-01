/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/smf.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <net/nrf_cloud.h>
#include <net/nrf_cloud_coap.h>
#include <net/nrf_cloud_rest.h>
#include <nrf_cloud_coap_transport.h>
#include <zephyr/net/coap.h>
#include <app_version.h>
#include <date_time.h>

#if defined(CONFIG_MEMFAULT)
#include <memfault/ports/zephyr/http.h>
#include <memfault/metrics/metrics.h>
#include <memfault/panics/coredump.h>
#endif /* CONFIG_MEMFAULT */

#include "cloud.h"
#include "app_common.h"
#include "network.h"
#include "location.h"

#if defined(CONFIG_APP_POWER)
#include "power.h"
#endif /* CONFIG_APP_POWER */

#if defined(CONFIG_APP_ENVIRONMENTAL)
#include "environmental.h"
#endif /* CONFIG_APP_ENVIRONMENTAL */

/* Register log module */
LOG_MODULE_REGISTER(cloud, CONFIG_APP_CLOUD_LOG_LEVEL);

#define CUSTOM_JSON_APPID_VAL_CONEVAL "CONEVAL"
#define CUSTOM_JSON_APPID_VAL_BATTERY "BATTERY"

#define AGNSS_MAX_DATA_SIZE 3800

BUILD_ASSERT(CONFIG_APP_CLOUD_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_CLOUD_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

/* Register zbus subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(cloud_subscriber);

/* Define the channels that the module subscribes to, their associated message types
 * and the subscriber that will receive the messages on the channel.
 * ENVIRONMENTAL_CHAN, POWER_CHAN, and LOCATION_CHAN are optional and are only included if the
 * corresponding module is enabled.
 */
#define CHANNEL_LIST(X)										\
					 X(NETWORK_CHAN,	struct network_msg)		\
					 X(CLOUD_CHAN,		struct cloud_msg)		\
IF_ENABLED(CONFIG_APP_ENVIRONMENTAL,	(X(ENVIRONMENTAL_CHAN,	struct environmental_msg)))	\
IF_ENABLED(CONFIG_APP_POWER,		(X(POWER_CHAN,		struct power_msg)))		\
IF_ENABLED(CONFIG_APP_LOCATION,		(X(LOCATION_CHAN,	struct location_msg)))

/* Calculate the maximum message size from the list of channels */
#define MAX_MSG_SIZE			MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

/* Add the cloud_subscriber as observer to all the channels in the list. */
#define ADD_OBSERVERS(_chan, _type)	ZBUS_CHAN_ADD_OBS(_chan, cloud_subscriber, 0);

/*
 * Expand to a call to ZBUS_CHAN_ADD_OBS for each channel in the list.
 * Example: ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, cloud_subscriber, 0);
 */
CHANNEL_LIST(ADD_OBSERVERS)

ZBUS_CHAN_DEFINE(CLOUD_CHAN,
		 struct cloud_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.type = CLOUD_DISCONNECTED)
);

/* Enumerator to be used in privat cloud channel */
enum priv_cloud_msg {
	CLOUD_BACKOFF_EXPIRED,
	CLOUD_SEND_REQUEST_FAILED,
};

/* Create private cloud channel for internal messaging that is not intended for external use.
 * The channel is needed to communicate from asynchronous callbacks to the state machine and
 * ensure state transitions only happen from the cloud  module thread where the state machine
 * is running.
 */
ZBUS_CHAN_DEFINE(PRIV_CLOUD_CHAN,
		 enum priv_cloud_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(cloud_subscriber),
		 CLOUD_BACKOFF_EXPIRED
);

/* Connection attempt backoff timer is run as a delayable work on the system workqueue */
static void backoff_timer_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(backoff_timer_work, backoff_timer_work_fn);

/* State machine */

/* Cloud module states */
enum cloud_module_state {
	/* The cloud module has started and is running */
	STATE_RUNNING,
		/* Cloud connection is not established */
		STATE_DISCONNECTED,
		/* The module is connecting to cloud */
		STATE_CONNECTING,
			/* The module is trying to connect to cloud */
			STATE_CONNECTING_ATTEMPT,
			/* The module is waiting before trying to connect again */
			STATE_CONNECTING_BACKOFF,
		/* Cloud connection has been established. Note that because of
		 * connection ID being used, the connection is valid even though
		 * network connection is intermittently lost (and socket is closed)
		 */
		STATE_CONNECTED,
			/* Connected to cloud and network connection, ready to send data */
			STATE_CONNECTED_READY,
			/* Connected to cloud, but not network connection */
			STATE_CONNECTED_PAUSED,
};

/* State object.
 * Used to transfer context data between state changes.
 */
struct cloud_state_object {
	/* This must be first */
	struct smf_ctx ctx;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;

	/* Last received message */
	uint8_t msg_buf[MAX_MSG_SIZE];

	/* Network status */
	enum network_msg_type nw_status;

	/* Connection attempt counter. Reset when entering STATE_CONNECTING */
	uint32_t connection_attempts;

	/* Connection backoff time */
	uint32_t backoff_time;
};

/* Forward declarations of state handlers */
static void state_running_entry(void *obj);
static void state_running_run(void *obj);
static void state_disconnected_entry(void *obj);
static void state_disconnected_run(void *obj);
static void state_connecting_entry(void *obj);
static void state_connecting_attempt_entry(void *obj);
static void state_connecting_backoff_entry(void *obj);
static void state_connecting_backoff_run(void *obj);
static void state_connecting_backoff_exit(void *obj);
static void state_connected_entry(void *obj);
static void state_connected_exit(void *obj);
static void state_connected_ready_entry(void *obj);
static void state_connected_ready_run(void *obj);
static void state_connected_paused_entry(void *obj);
static void state_connected_paused_run(void *obj);

/* State machine definition */
static const struct smf_state states[] = {
	[STATE_RUNNING] =
		SMF_CREATE_STATE(state_running_entry, state_running_run, NULL,
				 NULL, /* No parent state */
				 &states[STATE_DISCONNECTED]), /* Initial transition */

	[STATE_DISCONNECTED] =
		SMF_CREATE_STATE(state_disconnected_entry, state_disconnected_run, NULL,
				 &states[STATE_RUNNING],
				 NULL),

	[STATE_CONNECTING] =
		SMF_CREATE_STATE(state_connecting_entry, NULL, NULL,
				 &states[STATE_RUNNING],
				 &states[STATE_CONNECTING_ATTEMPT]),

	[STATE_CONNECTING_ATTEMPT] =
		SMF_CREATE_STATE(state_connecting_attempt_entry, NULL, NULL,
				 &states[STATE_CONNECTING],
				 NULL),

	[STATE_CONNECTING_BACKOFF] =
		SMF_CREATE_STATE(state_connecting_backoff_entry, state_connecting_backoff_run,
				 state_connecting_backoff_exit,
				 &states[STATE_CONNECTING],
				 NULL),

	[STATE_CONNECTED] =
		SMF_CREATE_STATE(state_connected_entry, NULL, state_connected_exit,
				 &states[STATE_RUNNING],
				 &states[STATE_CONNECTED_READY]),

	[STATE_CONNECTED_READY] =
		SMF_CREATE_STATE(state_connected_ready_entry, state_connected_ready_run, NULL,
				 &states[STATE_CONNECTED],
				 NULL),

	[STATE_CONNECTED_PAUSED] =
		SMF_CREATE_STATE(state_connected_paused_entry, state_connected_paused_run,  NULL,
				 &states[STATE_CONNECTED],
				 NULL),
};

static void cloud_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void connect_to_cloud(const struct cloud_state_object *state_object)
{
	int err;
	char buf[NRF_CLOUD_CLIENT_ID_MAX_LEN];

	err = nrf_cloud_client_id_get(buf, sizeof(buf));
	if (err == 0) {
		LOG_INF("Connecting to nRF Cloud CoAP with client ID: %s", buf);
	} else {
		LOG_ERR("nrf_cloud_client_id_get, error: %d, cannot continue", err);

		SEND_FATAL_ERROR();
		return;
	}

	err = nrf_cloud_coap_connect(APP_VERSION_STRING);
	if (err == 0) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED]);

		return;
	}

	/* Connection failed, retry */
	LOG_ERR("nrf_cloud_coap_connect, error: %d", err);

	smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTING_BACKOFF]);
}

static uint32_t calculate_backoff_time(uint32_t attempts)
{
	uint32_t backoff_time = CONFIG_APP_CLOUD_BACKOFF_INITIAL_SECONDS;

	/* Calculate backoff time */
	if (IS_ENABLED(CONFIG_APP_CLOUD_BACKOFF_TYPE_EXPONENTIAL)) {
		backoff_time = CONFIG_APP_CLOUD_BACKOFF_INITIAL_SECONDS << (attempts - 1);
	} else if (IS_ENABLED(CONFIG_APP_CLOUD_BACKOFF_TYPE_LINEAR)) {
		backoff_time = CONFIG_APP_CLOUD_BACKOFF_INITIAL_SECONDS +
			((attempts - 1) * CONFIG_APP_CLOUD_BACKOFF_LINEAR_INCREMENT_SECONDS);
	}

	/* Limit backoff time */
	if (backoff_time > CONFIG_APP_CLOUD_BACKOFF_MAX_SECONDS) {
		backoff_time = CONFIG_APP_CLOUD_BACKOFF_MAX_SECONDS;
	}

	LOG_DBG("Backoff time: %u seconds", backoff_time);

	return backoff_time;
}

static void backoff_timer_work_fn(struct k_work *work)
{
	int err;
	enum priv_cloud_msg msg = CLOUD_BACKOFF_EXPIRED;

	ARG_UNUSED(work);

	err = zbus_chan_pub(&PRIV_CLOUD_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void send_request_failed(void)
{
	int err;
	enum priv_cloud_msg cloud_msg = CLOUD_SEND_REQUEST_FAILED;

	err = zbus_chan_pub(&PRIV_CLOUD_CHAN, &cloud_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

/* State handlers */

static void state_running_entry(void *obj)
{
	int err;

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	err = nrf_cloud_coap_init();
	if (err) {
		LOG_ERR("nrf_cloud_coap_init, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static void state_running_run(void *obj)
{
	struct cloud_state_object const *state_object = obj;

	if (state_object->chan == &NETWORK_CHAN) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		if (msg.type == NETWORK_DISCONNECTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED]);

			return;
		}
	}
}

static void state_disconnected_entry(void *obj)
{
	int err;
	struct cloud_msg cloud_msg = {
		.type = CLOUD_DISCONNECTED,
	};

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static void state_disconnected_run(void *obj)
{
	struct cloud_state_object const *state_object = obj;
	struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

	if ((state_object->chan == &NETWORK_CHAN) && (msg.type == NETWORK_CONNECTED)) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTING]);

		return;
	}
}

static void state_connecting_entry(void *obj)
{
	/* Reset connection attempts counter */
	struct cloud_state_object *state_object = obj;

	LOG_DBG("%s", __func__);

	state_object->connection_attempts = 0;
}

static void state_connecting_attempt_entry(void *obj)
{
	struct cloud_state_object *state_object = obj;

	LOG_DBG("%s", __func__);

	state_object->connection_attempts++;

	connect_to_cloud(state_object);
}

static void state_connecting_backoff_entry(void *obj)
{
	int err;
	struct cloud_state_object *state_object = obj;

	LOG_DBG("%s", __func__);

	state_object->backoff_time = calculate_backoff_time(state_object->connection_attempts);

	err = k_work_schedule(&backoff_timer_work, K_SECONDS(state_object->backoff_time));
	if (err < 0) {
		LOG_ERR("k_work_schedule, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void state_connecting_backoff_run(void *obj)
{
	struct cloud_state_object const *state_object = obj;

	if (state_object->chan == &PRIV_CLOUD_CHAN) {
		const enum priv_cloud_msg msg = *(const enum priv_cloud_msg *)state_object->msg_buf;

		if (msg == CLOUD_BACKOFF_EXPIRED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTING_ATTEMPT]);

			return;
		}
	}
}

static void state_connecting_backoff_exit(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	(void)k_work_cancel_delayable(&backoff_timer_work);
}

static void state_connected_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);
	LOG_INF("Connected to Cloud");

#if defined(CONFIG_MEMFAULT)
	if (memfault_coredump_has_valid_coredump(NULL)) {
		/* Initial update to Memfault is handled internally in the
		 * Memfault LTE coredump layer.
		 */
		return;
	}

	/* No coredump available, trigger an initial update to Memfault. */
	(void)memfault_metrics_heartbeat_debug_trigger();
	(void)memfault_zephyr_port_post_data();
#endif /* CONFIG_MEMFAULT */
}

static void state_connected_exit(void *obj)
{
	int err;

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	err = nrf_cloud_coap_disconnect();
	if (err && (err != -ENOTCONN && err != -EPERM)) {
		LOG_ERR("nrf_cloud_coap_disconnect, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

#if defined(CONFIG_APP_LOCATION)
/* Handle cloud location requests from the location module */
static void handle_cloud_location_request(const struct location_data_cloud *request)
{
	int err;
	struct location_data location = { 0 };
	struct nrf_cloud_rest_location_request loc_req = { 0 };
	struct nrf_cloud_location_result result = { 0 };

	LOG_DBG("Handling cloud location request");

#if defined(CONFIG_LOCATION_METHOD_CELLULAR)
	if (request->cell_data != NULL) {
		/* NOSONAR: Cast away const qualifier is required due to API design mismatch
		 * between location library (const pointers) and nRF Cloud API (non-const pointers).
		 * The underlying nrf_cloud_coap_location_get function only reads the data.
		 */
		loc_req.cell_info = (struct lte_lc_cells_info *)request->cell_data; /* NOSONAR */

		LOG_DBG("Cellular data present: current cell ID: %d, neighbor cells: %d",
			request->cell_data->current_cell.id,
			request->cell_data->ncells_count);
	}
#endif

#if defined(CONFIG_LOCATION_METHOD_WIFI)
	if (request->wifi_data != NULL) {
		/* NOSONAR: Cast away const qualifier is required due to API design mismatch
		 * between location library (const pointers) and nRF Cloud API (non-const pointers).
		 * The underlying nrf_cloud_coap_location_get function only reads the data.
		 */
		loc_req.wifi_info = (struct wifi_scan_info *)request->wifi_data; /* NOSONAR */

		LOG_DBG("Wi-Fi data present: %d APs", request->wifi_data->cnt);
	}
#endif

	/* Send location request to nRF Cloud */
	err = nrf_cloud_coap_location_get(&loc_req, &result);
	if (err == COAP_RESPONSE_CODE_NOT_FOUND) {
		LOG_WRN("nRF Cloud CoAP location coordinates not found, error: %d", err);
		location_cloud_location_ext_result_set(LOCATION_EXT_RESULT_ERROR, NULL);

		return;
	} else if (err) {
		LOG_ERR("nrf_cloud_coap_location_get, error: %d", err);
		location_cloud_location_ext_result_set(LOCATION_EXT_RESULT_ERROR, NULL);

		send_request_failed();
		return;
	}

	LOG_INF("Location result: lat: %f, lon: %f, accuracy: %f",
		result.lat, result.lon, (double)result.unc);

	/* Convert result to location_data format */
	location.latitude = result.lat;
	location.longitude = result.lon;
	location.accuracy = (double)result.unc;

	/* Send successful result back to location library */
	location_cloud_location_ext_result_set(LOCATION_EXT_RESULT_SUCCESS, &location);
}

#if defined(CONFIG_NRF_CLOUD_AGNSS)
/* Handle A-GNSS data requests from the location module */
static void handle_agnss_request(const struct nrf_modem_gnss_agnss_data_frame *request)
{
	int err;
	static char agnss_buf[AGNSS_MAX_DATA_SIZE];
	struct nrf_cloud_rest_agnss_request agnss_req = {
		.type = NRF_CLOUD_REST_AGNSS_REQ_CUSTOM,
		.agnss_req = (struct nrf_modem_gnss_agnss_data_frame *)request,
		.net_info = NULL,
		.filtered = false,
		.mask_angle = 0
	};
	struct nrf_cloud_rest_agnss_result result = {
		.buf = agnss_buf,
		.buf_sz = sizeof(agnss_buf),
		.agnss_sz = 0
	};

	LOG_DBG("Handling A-GNSS data request");

	/* Send A-GNSS request to nRF Cloud */
	err = nrf_cloud_coap_agnss_data_get(&agnss_req, &result);
	if (err) {
		LOG_ERR("nrf_cloud_coap_agnss_data_get, error: %d", err);

		send_request_failed();
		return;
	}

	LOG_DBG("A-GNSS data received, size: %d bytes", result.agnss_sz);

	/* Process the A-GNSS data */
	err = location_agnss_data_process(result.buf, result.agnss_sz);
	if (err) {
		LOG_ERR("Failed to process A-GNSS data, error: %d", err);
		return;
	}

	LOG_DBG("A-GNSS data processed successfully");
}
#endif /* CONFIG_NRF_CLOUD_AGNSS */

#if defined(CONFIG_LOCATION_METHOD_GNSS)
/* Handle GNSS location data from the location module */
static void handle_gnss_location_data(const struct location_data *location_data)
{
	int err;
	int64_t timestamp_ms = NRF_CLOUD_NO_TIMESTAMP;
	bool confirmable = IS_ENABLED(CONFIG_APP_CLOUD_CONFIRMABLE_MESSAGES);
	struct nrf_cloud_gnss_data gnss_data = {
		.type = NRF_CLOUD_GNSS_TYPE_PVT,
		.ts_ms = timestamp_ms,
		.pvt = {
			.lat = location_data->latitude,
			.lon = location_data->longitude,
			.accuracy = location_data->accuracy,
		}
	};

	LOG_DBG("Handling GNSS location data: lat: %f, lon: %f, acc: %f",
		(double)location_data->latitude,
		(double)location_data->longitude,
		(double)location_data->accuracy);

	/* Get current timestamp */
	err = date_time_now(&timestamp_ms);
	if (err) {
		LOG_WRN("Failed to get current time");

		timestamp_ms = NRF_CLOUD_NO_TIMESTAMP;
	}

	gnss_data.ts_ms = timestamp_ms;

#if defined(CONFIG_LOCATION_DATA_DETAILS)
#define CLOUD_GNSS_HEADING_ACC_LIMIT (float)60.0

	/* Shorten the name of the struct to make the code more readable */
	struct location_data_details_gnss gnss = location_data->details.gnss;

	/* If detailed GNSS data is available, include altitude, speed, and heading */
	if (gnss.pvt_data.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
		gnss_data.pvt.alt = gnss.pvt_data.altitude;
		gnss_data.pvt.speed = gnss.pvt_data.speed;
		gnss_data.pvt.heading = gnss.pvt_data.heading;
		gnss_data.pvt.has_alt = 1;
		gnss_data.pvt.has_speed =
			(gnss.pvt_data.flags & NRF_MODEM_GNSS_PVT_FLAG_VELOCITY_VALID) ? 1 : 0;
		gnss_data.pvt.has_heading =
			(gnss.pvt_data.heading_accuracy < CLOUD_GNSS_HEADING_ACC_LIMIT) ? 1 : 0;
	}
#endif /* CONFIG_LOCATION_DATA_DETAILS */

	/* Send GNSS location data to nRF Cloud */
	err = nrf_cloud_coap_location_send(&gnss_data, confirmable);
	if (err) {
		LOG_ERR("nrf_cloud_coap_location_send, error: %d", err);
		send_request_failed();
		return;
	}

	LOG_INF("GNSS location data sent to nRF Cloud successfully");
}
#endif /* CONFIG_LOCATION_METHOD_GNSS */
#endif /* CONFIG_APP_LOCATION */

static void shadow_get(bool delta_only)
{
	int err;
	struct cloud_msg msg = {
		.type = CLOUD_SHADOW_RESPONSE,
		.response = {
			.buffer_data_len = sizeof(msg.response.buffer),
		},
	};

	LOG_DBG("Requesting device shadow from the device");

	err = nrf_cloud_coap_shadow_get(msg.response.buffer,
					&msg.response.buffer_data_len,
					delta_only,
					COAP_CONTENT_FORMAT_APP_CBOR);
	if (err) {
		LOG_ERR("nrf_cloud_coap_shadow_get, error: %d", err);

		send_request_failed();
		return;
	}

	if (msg.response.buffer_data_len == 0) {
		LOG_DBG("No shadow delta changes available");
		return;
	}

	/* Workaroud: Sometimes nrf_cloud_coap_shadow_get() returns 0 even though obtaining
	 * the shadow failed. Ignore the payload if the first 10 bytes are zero.
	 */
	if (!memcmp(msg.response.buffer, "\0\0\0\0\0\0\0\0\0\0", 10)) {
		LOG_WRN("Returned buffeør is empty, ignore");
		return;
	}

	err = zbus_chan_pub(&CLOUD_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	/* Clear the shadow delta by reporting the same data back to the shadow reported state  */
	err = nrf_cloud_coap_patch("state/reported", NULL,
				   msg.response.buffer,
				   msg.response.buffer_data_len,
				   COAP_CONTENT_FORMAT_APP_CBOR,
				   true,
				   NULL,
				   NULL);
	if (err) {
		LOG_ERR("nrf_cloud_coap_patch, error: %d", err);

		send_request_failed();
		return;
	}
}

static void state_connected_ready_entry(void *obj)
{
	int err;
	struct cloud_msg cloud_msg = {
		.type = CLOUD_CONNECTED,
	};

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	shadow_get(false);
}

static void state_connected_ready_run(void *obj)
{
	int err;
	struct cloud_state_object const *state_object = obj;
	bool confirmable = IS_ENABLED(CONFIG_APP_CLOUD_CONFIRMABLE_MESSAGES);

	if (state_object->chan == &PRIV_CLOUD_CHAN) {
		enum priv_cloud_msg msg = *(const enum priv_cloud_msg *)state_object->msg_buf;

		if (msg == CLOUD_SEND_REQUEST_FAILED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTING]);

			return;
		}
	}

	if (state_object->chan == &NETWORK_CHAN) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_DISCONNECTED:
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_PAUSED]);

			return;
		case NETWORK_CONNECTED:
			smf_set_handled(SMF_CTX(state_object));

			return;
		case NETWORK_QUALITY_SAMPLE_RESPONSE:
			err = nrf_cloud_coap_sensor_send(CUSTOM_JSON_APPID_VAL_CONEVAL,
							 msg.conn_eval_params.energy_estimate,
							 NRF_CLOUD_NO_TIMESTAMP,
							 confirmable);
			if (err) {
				LOG_ERR("nrf_cloud_coap_sensor_send, error: %d", err);

				send_request_failed();
				return;
			}

			err = nrf_cloud_coap_sensor_send(NRF_CLOUD_JSON_APPID_VAL_RSRP,
							 msg.conn_eval_params.rsrp,
							 NRF_CLOUD_NO_TIMESTAMP,
							 confirmable);
			if (err) {
				LOG_ERR("nrf_cloud_coap_sensor_send, error: %d", err);

				send_request_failed();
				return;
			}

			break;

		default:
			break;
		}
	}

#if defined(CONFIG_APP_POWER)
	if (state_object->chan == &POWER_CHAN) {
		struct power_msg msg = MSG_TO_POWER_MSG(state_object->msg_buf);

		if (msg.type == POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE) {
			err = nrf_cloud_coap_sensor_send(CUSTOM_JSON_APPID_VAL_BATTERY,
							 msg.percentage,
							 NRF_CLOUD_NO_TIMESTAMP,
							 confirmable);
			if (err) {
				LOG_ERR("nrf_cloud_coap_sensor_send, error: %d", err);

				send_request_failed();
				return;
			}

			return;
		}
	}
#endif /* CONFIG_APP_POWER */

#if defined(CONFIG_APP_ENVIRONMENTAL)
	if (state_object->chan == &ENVIRONMENTAL_CHAN) {
		struct environmental_msg msg = MSG_TO_ENVIRONMENTAL_MSG(state_object->msg_buf);

		if (msg.type == ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE) {
			err = nrf_cloud_coap_sensor_send(NRF_CLOUD_JSON_APPID_VAL_TEMP,
							 msg.temperature,
							 NRF_CLOUD_NO_TIMESTAMP,
							 confirmable);
			if (err) {
				LOG_ERR("nrf_cloud_coap_sensor_send, error: %d", err);

				send_request_failed();
				return;
			}

			err = nrf_cloud_coap_sensor_send(NRF_CLOUD_JSON_APPID_VAL_AIR_PRESS,
							 msg.pressure,
							 NRF_CLOUD_NO_TIMESTAMP,
							 confirmable);
			if (err) {
				LOG_ERR("nrf_cloud_coap_sensor_send, error: %d", err);

				send_request_failed();
				return;
			}

			err = nrf_cloud_coap_sensor_send(NRF_CLOUD_JSON_APPID_VAL_HUMID,
							 msg.humidity,
							 NRF_CLOUD_NO_TIMESTAMP,
							 confirmable);
			if (err) {
				LOG_ERR("nrf_cloud_coap_sensor_send, error: %d", err);

				send_request_failed();
				return;
			}

			return;
		}

		return;
	}
#endif /* CONFIG_APP_ENVIRONMENTAL */

#if defined(CONFIG_APP_LOCATION)
	if (state_object->chan == &LOCATION_CHAN) {
		const struct location_msg *msg = MSG_TO_LOCATION_MSG_PTR(state_object->msg_buf);

		switch (msg->type) {
		case LOCATION_CLOUD_REQUEST:
			LOG_DBG("Cloud location request received");
			handle_cloud_location_request(&msg->cloud_request);
			break;

#if defined(CONFIG_NRF_CLOUD_AGNSS)
		case LOCATION_AGNSS_REQUEST:
			LOG_DBG("A-GNSS data request received");
			handle_agnss_request(&msg->agnss_request);
			break;
#endif /* CONFIG_NRF_CLOUD_AGNSS */

#if defined(CONFIG_LOCATION_METHOD_GNSS)
		case LOCATION_GNSS_DATA:
			LOG_DBG("GNSS location data received");
			handle_gnss_location_data(&msg->gnss_data);
			break;
#endif /* CONFIG_LOCATION_METHOD_GNSS */

		default:
			break;
		}

		return;
	}
#endif /* CONFIG_APP_LOCATION */

	if (state_object->chan == &CLOUD_CHAN) {
		const struct cloud_msg *msg = MSG_TO_CLOUD_MSG_PTR(state_object->msg_buf);

		if (msg->type == CLOUD_PAYLOAD_JSON) {
			err = nrf_cloud_coap_json_message_send(msg->payload.buffer,
							       false, confirmable);
			if (err) {
				LOG_ERR("nrf_cloud_coap_json_message_send, error: %d", err);

				send_request_failed();
				return;
			}
		} else if (msg->type == CLOUD_POLL_SHADOW) {
			LOG_DBG("Poll shadow trigger received");

			shadow_get(true);
		}
	}
}

/* Handlers for STATE_CONNECTED_PAUSED */

static void state_connected_paused_entry(void *obj)
{
	int err;
	struct cloud_msg cloud_msg = {
		.type = CLOUD_DISCONNECTED,
	};

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static void state_connected_paused_run(void *obj)
{
	struct cloud_state_object const *state_object = obj;
	struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

	if ((state_object->chan == &NETWORK_CHAN) && (msg.type == NETWORK_CONNECTED)) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_READY]);

		return;
	}
}

static void cloud_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = (CONFIG_APP_CLOUD_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_CLOUD_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	struct cloud_state_object cloud_state = { 0 };

	LOG_DBG("Cloud module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, cloud_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		SEND_FATAL_ERROR();
		return;
	}

	/* Initialize the state machine to STATE_RUNNING, which will also run its entry function */
	smf_set_initial(SMF_CTX(&cloud_state), &states[STATE_RUNNING]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}

		err = zbus_sub_wait_msg(&cloud_subscriber, &cloud_state.chan, cloud_state.msg_buf,
					zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}

		err = smf_run_state(SMF_CTX(&cloud_state));
		if (err) {
			LOG_ERR("smf_run_state(), error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}
	}
}

K_THREAD_DEFINE(cloud_module_thread_id,
		CONFIG_APP_CLOUD_THREAD_STACK_SIZE,
		cloud_module_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
