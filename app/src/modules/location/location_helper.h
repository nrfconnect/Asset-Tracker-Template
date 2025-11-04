/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _LOCATION_HELPER_H_
#define _LOCATION_HELPER_H_

#include "location.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Copy location data from location library format to location module format.
 *
 * @param[out] dest Destination structure
 * @param[in] src Source structure from location library
 *
 * @return 0 on success, negative error code on failure
 * @retval -EINVAL if parameters are invalid
 * @retval -ENOMEM if the destination structure is too small to copy all data, e.g.,
 *		   not enough space for all neighbor cells or Wi-Fi APs.
 */
int location_cloud_request_data_copy(struct location_cloud_request_data *dest,
				     const struct location_data_cloud *src);

#ifdef __cplusplus
}
#endif

#endif /* _LOCATION_HELPER_H_ */
