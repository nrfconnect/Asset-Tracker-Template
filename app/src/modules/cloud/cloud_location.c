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

#if defined(CONFIG_LOCATION_METHOD_CELLULAR)
static int cellular_cell_data_construct(struct lte_lc_cells_info *dest,
					struct lte_lc_ncell *neighbor_cells,
					size_t neighbor_cells_count,
					struct lte_lc_cell *gci_cells,
					size_t gci_cells_count,
					const struct location_cloud_request_data *src)
{
	if (!dest || !src || !neighbor_cells || !gci_cells) {
		LOG_ERR("Invalid NULL parameter(s) provided");
		return -EINVAL;
	}

	if (neighbor_cells_count < src->ncells_count) {
		LOG_ERR("Insufficient neighbor_cells_count: %zu, required: %d",
			neighbor_cells_count, src->ncells_count);
		return -ENOMEM;
	}

	if (gci_cells_count < src->gci_cells_count) {
		LOG_ERR("Insufficient gci_cells_count: %zu, required: %d",
			gci_cells_count, src->gci_cells_count);
		return -ENOMEM;
	}

	/* Copy current cell information */
	dest->current_cell.mcc = src->current_cell.mcc;
	dest->current_cell.mnc = src->current_cell.mnc;
	dest->current_cell.id = src->current_cell.id;
	dest->current_cell.tac = src->current_cell.tac;
	dest->current_cell.timing_advance = src->current_cell.timing_advance;
	dest->current_cell.earfcn = src->current_cell.earfcn;
	dest->current_cell.rsrp = src->current_cell.rsrp;
	dest->current_cell.rsrq = src->current_cell.rsrq;

	dest->ncells_count = src->ncells_count;
	dest->gci_cells_count = src->gci_cells_count;

	/* Copy neighbor cells */
	for (uint8_t i = 0; i < src->ncells_count; i++) {
		neighbor_cells[i].earfcn = src->neighbor_cells[i].earfcn;
		neighbor_cells[i].time_diff = src->neighbor_cells[i].time_diff;
		neighbor_cells[i].phys_cell_id = src->neighbor_cells[i].phys_cell_id;
		neighbor_cells[i].rsrp = src->neighbor_cells[i].rsrp;
		neighbor_cells[i].rsrq = src->neighbor_cells[i].rsrq;
	}

	dest->neighbor_cells = neighbor_cells;

	/* Copy GCI cells */
	for (uint8_t i = 0; i < src->gci_cells_count; i++) {
		gci_cells[i].id = src->gci_cells[i].id;
		gci_cells[i].mcc = src->gci_cells[i].mcc;
		gci_cells[i].mnc = src->gci_cells[i].mnc;
		gci_cells[i].tac = src->gci_cells[i].tac;
		gci_cells[i].timing_advance = src->gci_cells[i].timing_advance;
		gci_cells[i].earfcn = src->gci_cells[i].earfcn;
		gci_cells[i].rsrp = src->gci_cells[i].rsrp;
		gci_cells[i].rsrq = src->gci_cells[i].rsrq;
	}

	dest->gci_cells = gci_cells;

	return 0;
}
#endif	/* CONFIG_LOCATION_METHOD_CELLULAR */

#if defined(CONFIG_LOCATION_METHOD_WIFI)
static int wifi_ap_data_construct(struct wifi_scan_info *dest,
				  struct wifi_scan_result *ap_info,
				  size_t ap_info_count,
				  const struct location_cloud_request_data *src)
{
	if (!dest || !src || !ap_info) {
		LOG_ERR("Invalid NULL parameter(s) provided");
		return -EINVAL;
	}

	if (ap_info_count < src->wifi_cnt) {
		LOG_ERR("Insufficient ap_info_count: %zu, required: %d",
			ap_info_count, src->wifi_cnt);
		return -ENOMEM;
	}

	if (sizeof(ap_info[0].mac) < sizeof(src->wifi_aps[0].mac)) {
		LOG_ERR("Insufficient MAC array size in wifi_scan_result");
		return -EINVAL;
	}

	/* Copy WiFi AP data */
	for (uint16_t i = 0; i < src->wifi_cnt; i++) {
		ap_info[i].rssi = src->wifi_aps[i].rssi;
		memcpy(ap_info[i].mac,
		       src->wifi_aps[i].mac,
		       WIFI_MAC_ADDR_LEN);
		ap_info[i].mac_length = src->wifi_aps[i].mac_length;
	}

	dest->ap_info = ap_info;
	dest->cnt = src->wifi_cnt;

	return 0;
}
#endif /* CONFIG_LOCATION_METHOD_WIFI */

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
static void handle_cloud_location_request(const struct location_cloud_request_data *request)
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
	struct lte_lc_cells_info cell_info = { 0 };
	struct lte_lc_ncell neighbor_cells[CONFIG_LTE_NEIGHBOR_CELLS_MAX];
	struct lte_lc_cell gci_cells[CONFIG_LTE_NEIGHBOR_CELLS_MAX];

	if ((request->current_cell.id != LTE_LC_CELL_EUTRAN_ID_INVALID) &&
	    (request->ncells_count > 0)) {
		err = cellular_cell_data_construct(&cell_info, neighbor_cells,
						   ARRAY_SIZE(neighbor_cells),
						   gci_cells,
						   ARRAY_SIZE(gci_cells),
						   request);
		if (err) {
			LOG_ERR("Failed to reconstruct cellular data, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		loc_req.cell_info = &cell_info;
	}
#endif /* CONFIG_LOCATION_METHOD_CELLULAR */

#if defined(CONFIG_LOCATION_METHOD_WIFI)
	struct wifi_scan_result ap_info[CONFIG_LOCATION_METHOD_WIFI_SCANNING_RESULTS_MAX_CNT];
	struct wifi_scan_info wifi_info = { 0 };

	if (request->wifi_cnt > 0) {
		err = wifi_ap_data_construct(&wifi_info, ap_info, ARRAY_SIZE(ap_info), request);
		if (err) {
			LOG_ERR("Failed to reconstruct Wi-Fi data, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		loc_req.wifi_info = &wifi_info;
	}
#endif /* CONFIG_LOCATION_METHOD_WIFI */

	if (!loc_req.cell_info && !loc_req.wifi_info) {
		LOG_ERR("No cellular or Wi-Fi data provided for location request");
		return;
	}

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
static void handle_gnss_location_data(const struct location_msg *location_msg)
{
	int err;
	int64_t timestamp_ms = NRF_CLOUD_NO_TIMESTAMP;
	bool confirmable = IS_ENABLED(CONFIG_APP_CLOUD_CONFIRMABLE_MESSAGES);
	const struct location_data *location_data = &location_msg->gnss_data;

	/* Convert uptime to unix time */
	timestamp_ms = location_msg->timestamp;
	err = date_time_uptime_to_unix_time_ms(&timestamp_ms);
	if (err) {
		LOG_ERR("date_time_uptime_to_unix_time_ms, error: %d", err);
	}

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
		handle_gnss_location_data(msg);
		break;
#endif /* CONFIG_LOCATION_METHOD_GNSS */

	default:
		break;
	}
}
