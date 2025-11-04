/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "location_helper.h"

LOG_MODULE_DECLARE(location_module, CONFIG_APP_LOCATION_LOG_LEVEL);

#if defined(CONFIG_LOCATION_METHOD_CELLULAR)
static int copy_cellular_data(struct location_cloud_request_data *dest,
			      const struct lte_lc_cells_info *src)
{
	if ((src == NULL) || (dest == NULL)) {
		LOG_ERR("Invalid cellular data");
		return -EINVAL;
	}

	if ((src->ncells_count > ARRAY_SIZE(dest->neighbor_cells)) ||
	    (src->gci_cells_count > ARRAY_SIZE(dest->gci_cells))) {
		LOG_ERR("Not enough memory for cellular data");
		return -ENOMEM;
	}

	/* Copy current cell information */
	dest->current_cell.id = src->current_cell.id;
	dest->current_cell.mcc = src->current_cell.mcc;
	dest->current_cell.mnc = src->current_cell.mnc;
	dest->current_cell.tac = src->current_cell.tac;
	dest->current_cell.timing_advance = src->current_cell.timing_advance;
	dest->current_cell.earfcn = src->current_cell.earfcn;
	dest->current_cell.rsrp = src->current_cell.rsrp;
	dest->current_cell.rsrq = src->current_cell.rsrq;

	/* Copy neighbor cells */
	for (uint8_t i = 0; i < src->ncells_count; i++) {
		dest->neighbor_cells[i].earfcn = src->neighbor_cells[i].earfcn;
		dest->neighbor_cells[i].time_diff = src->neighbor_cells[i].time_diff;
		dest->neighbor_cells[i].phys_cell_id = src->neighbor_cells[i].phys_cell_id;
		dest->neighbor_cells[i].rsrp = src->neighbor_cells[i].rsrp;
		dest->neighbor_cells[i].rsrq = src->neighbor_cells[i].rsrq;
	}

	dest->ncells_count = src->ncells_count;

	LOG_DBG("Copied %d neighbor cells", dest->ncells_count);

	/* Copy GCI cells if present */
	for (uint8_t i = 0; i < src->gci_cells_count && i < CONFIG_LTE_NEIGHBOR_CELLS_MAX; i++) {
		dest->gci_cells[i].id = src->gci_cells[i].id;
		dest->gci_cells[i].mcc = src->gci_cells[i].mcc;
		dest->gci_cells[i].mnc = src->gci_cells[i].mnc;
		dest->gci_cells[i].tac = src->gci_cells[i].tac;
		dest->gci_cells[i].timing_advance = src->gci_cells[i].timing_advance;
		dest->gci_cells[i].earfcn = src->gci_cells[i].earfcn;
		dest->gci_cells[i].rsrp = src->gci_cells[i].rsrp;
		dest->gci_cells[i].rsrq = src->gci_cells[i].rsrq;
	}

	dest->gci_cells_count = src->gci_cells_count;

	LOG_DBG("Copied %d GCI cells", dest->gci_cells_count);

	return 0;
}
#endif /* CONFIG_LOCATION_METHOD_CELLULAR */

#if defined(CONFIG_LOCATION_METHOD_WIFI)
static int copy_wifi_data(struct location_cloud_request_data *dest,
			   const struct wifi_scan_info *src)
{
	if ((src == NULL) || (src->ap_info == NULL) || (src->cnt == 0)) {
		LOG_ERR("Invalid WiFi scan info");
		return -EINVAL;
	}

	if (dest == NULL) {
		LOG_ERR("Invalid destination for WiFi data");
		return -EINVAL;
	}

	if (src->cnt > ARRAY_SIZE(dest->wifi_aps)) {
		return -ENOMEM;
	}

	for (uint8_t i = 0; i < src->cnt; i++) {
		dest->wifi_aps[i].rssi = src->ap_info[i].rssi;
		memcpy(dest->wifi_aps[i].mac,
		       src->ap_info[i].mac,
		       WIFI_MAC_ADDR_LEN);
		dest->wifi_aps[i].mac_length = src->ap_info[i].mac_length;
	}

	dest->wifi_cnt = src->cnt;

	LOG_DBG("Copied %d WiFi APs", dest->wifi_cnt);

	return 0;
}
#endif /* CONFIG_LOCATION_METHOD_WIFI */

int location_cloud_request_data_copy(struct location_cloud_request_data *dest,
				     const struct location_data_cloud *src)
{
	int err = 0;

	if (dest == NULL || src == NULL) {
		LOG_ERR("Invalid parameters for cloud request data copy");
		return -EINVAL;
	}

	LOG_DBG("Copying cloud request data, size of dest: %zu", sizeof(*dest));

#if defined(CONFIG_LOCATION_METHOD_CELLULAR)
	if (src->cell_data) {
		err = copy_cellular_data(dest, src->cell_data);
		if (err) {
			LOG_ERR("Failed to copy cellular data");
			return err;
		}
	}
#endif

#if defined(CONFIG_LOCATION_METHOD_WIFI)
	if (src->wifi_data) {
		err = copy_wifi_data(dest, src->wifi_data);
		if (err) {
			LOG_ERR("Failed to copy WiFi data");
			return err;
		}
	}
#endif

	return 0;
}
