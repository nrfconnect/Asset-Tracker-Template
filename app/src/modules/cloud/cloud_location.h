/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _CLOUD_LOCATION_H_
#define _CLOUD_LOCATION_H_

#include "location.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Handle location-related messages from the location module.
 *
 * Processes location requests, A-GNSS requests, and GNSS location data.
 *
 * @param msg Pointer to location message
 */
void cloud_location_handle_message(const struct location_msg *msg);

#if defined(CONFIG_NRF_CLOUD_AGNSS)
/**
 * @brief Cache an A-GNSS request for later processing.
 *
 * Use this when an A-GNSS request arrives while the cloud module is not
 * in connected-ready state. The cached request will be processed when
 * cloud_location_agnss_process_cached() is called.
 *
 * @param msg Pointer to the location message containing the A-GNSS request.
 */
void cloud_location_agnss_cache(const struct location_msg *msg);

/**
 * @brief Process a previously cached A-GNSS request.
 *
 * Should be called when the cloud module enters connected-ready state.
 * If no request is cached, this function does nothing.
 */
void cloud_location_agnss_process_cached(void);
#endif /* CONFIG_NRF_CLOUD_AGNSS */

#ifdef __cplusplus
}
#endif

#endif /* _CLOUD_LOCATION_H_ */
