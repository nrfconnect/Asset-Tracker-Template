/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _CLOUD_ENVIRONMENTAL_H_
#define _CLOUD_ENVIRONMENTAL_H_

#include <stdint.h>
#include "environmental.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Send environmental data to the cloud.
 *
 * Sends temperature, pressure, and humidity data to nRF Cloud.
 *
 * @param env Pointer to environmental message containing sensor data
 * @param timestamp_ms Timestamp in milliseconds, or NRF_CLOUD_NO_TIMESTAMP
 * @param confirmable Whether to use confirmable CoAP messages
 *
 * @return 0 on success, negative error code on failure
 */
int cloud_environmental_send(const struct environmental_msg *env,
			     int64_t timestamp_ms,
			     bool confirmable);

#ifdef __cplusplus
}
#endif

#endif /* _CLOUD_ENVIRONMENTAL_H_ */
