/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _CLOUD_CONFIGURATION_H_
#define _CLOUD_CONFIGURATION_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Types of sections of the shadow document to poll for.
 */
enum shadow_poll_type {
	/** Request the delta section (difference between reported and desired states) */
	SHADOW_POLL_DELTA,
	/** Request the desired section (target configuration from cloud) */
	SHADOW_POLL_DESIRED,
};

/**
 * @brief Poll the device shadow for configuration updates.
 *
 * Requests the desired or delta section of the device shadow from nRF Cloud.
 * The response is published on CLOUD_CHAN as either:
 * - CLOUD_SHADOW_RESPONSE_DESIRED or CLOUD_SHADOW_RESPONSE_EMPTY_DESIRED (for desired section)
 * - CLOUD_SHADOW_RESPONSE_DELTA or CLOUD_SHADOW_RESPONSE_EMPTY_DELTA (for delta section)
 *
 * @param type Type of shadow section to poll (SHADOW_POLL_DELTA or SHADOW_POLL_DESIRED)
 *
 * @return 0 on success, negative error code on failure
 */
int cloud_configuration_poll(enum shadow_poll_type type);

/**
 * @brief Update the reported section of the device shadow.
 *
 * Sends device configuration or command acknowledgment to the cloud's shadow
 * reported section using CBOR format.
 *
 * @param buffer Pointer to CBOR-encoded payload buffer
 * @param buffer_len Length of the payload data in bytes
 *
 * @return 0 on success, negative error code on failure
 */
int cloud_configuration_reported_update(const uint8_t *buffer, size_t buffer_len);

#ifdef __cplusplus
}
#endif

#endif /* _CLOUD_CONFIGURATION_H_ */
