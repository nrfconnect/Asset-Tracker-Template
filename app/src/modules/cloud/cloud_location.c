/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <net/nrf_cloud_coap.h>
#include <net/nrf_cloud_rest.h>
#include <zephyr/net/coap.h>
#include <date_time.h>

#include "cloud_location.h"
#include "cloud_internal.h"
#include "app_common.h"
#include "location.h"

LOG_MODULE_DECLARE(cloud, CONFIG_APP_CLOUD_LOG_LEVEL);

#define AGNSS_MAX_DATA_SIZE 3800

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

/* Handle cloud location requests from the location module */
static void handle_cloud_location_request(const struct location_data_cloud *request)
{
	int err;
	struct nrf_cloud_location_config loc_config = {
		.do_reply = false,
	};
	struct nrf_cloud_rest_location_request loc_req = {
		.config = &loc_config,
	};
	struct nrf_cloud_location_result result = { 0 };

	LOG_DBG("Handling cloud location request");

#if defined(CONFIG_LOCATION_METHOD_CELLULAR)
	if (request->cell_data != NULL) {
		/* Cast away const: nRF Cloud API limitation - struct fields are non-const
		 * but the data is not modified by the API
		 */
		loc_req.cell_info = (struct lte_lc_cells_info *)request->cell_data; /* NOSONAR */

		LOG_DBG("Cellular data present: current cell ID: %d, neighbor cells: %d, "
			"GCI cells count: %d",
			request->cell_data->current_cell.id,
			request->cell_data->ncells_count,
			request->cell_data->gci_cells_count);
	}
#endif

#if defined(CONFIG_LOCATION_METHOD_WIFI)
	if (request->wifi_data != NULL && request->wifi_data->cnt > 0) {
		/* Cast away const: nRF Cloud API limitation - struct fields are non-const
		 * but the data is not modified by the API
		 */
		loc_req.wifi_info = (struct wifi_scan_info *)request->wifi_data; /* NOSONAR */

		LOG_DBG("Wi-Fi data present: %d APs", request->wifi_data->cnt);
	} else if (request->wifi_data != NULL && request->wifi_data->cnt == 0) {
		LOG_DBG("Wi-Fi scan found 0 APs, omitting Wi-Fi data from location request");
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
}

#if defined(CONFIG_NRF_CLOUD_AGNSS)
/* Handle A-GNSS data requests from the location module */
static void handle_agnss_request(const struct nrf_modem_gnss_agnss_data_frame *request)
{
	int err;
	static char agnss_buf[AGNSS_MAX_DATA_SIZE];
	/* Cast away const: nRF Cloud API limitation - struct fields are non-const
	 * but the data is not modified by the API
	 */
	struct nrf_cloud_rest_agnss_request agnss_req = {
		.type = NRF_CLOUD_REST_AGNSS_REQ_CUSTOM,
		.agnss_req = (struct nrf_modem_gnss_agnss_data_frame *)request, /* NOSONAR */
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

void cloud_location_handle_message(const struct location_msg *msg)
{
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
}
