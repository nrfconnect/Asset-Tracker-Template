/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/types.h>

#define CLOUD_COMMAND_TYPE_PROVISION 1
#define CLOUD_COMMAND_TYPE_REBOOT 2

/**
 * @brief Get the device shadow parameters from a CBOR buffer.
 *
 * @param[in]  cbor              The CBOR buffer.
 * @param[in]  len               The length of the CBOR buffer.
 * @param[out] update_interval   Update interval in seconds.
 * @param[out] sample_interval   Sample interval in seconds.
 * @param[out] command_type      Cloud command type.
 *
 * @returns 0 If the operation was successful.
 *	    Otherwise, a (negative) error code is returned.
 * @retval -EFAULT if the CBOR buffer is invalid.
 *
 */
int get_parameters_from_cbor_response(const uint8_t *cbor,
				      size_t len,
				      uint32_t *update_interval,
				      uint32_t *sample_interval,
				      uint32_t *command_type);

/**
 * @brief Encode device configuration parameters to CBOR for shadow reporting.
 *
 * @param[in]  update_interval  Update interval in seconds.
 * @param[in]  sample_interval  Sample interval in seconds.
 * @param[out] buffer           Buffer to store encoded CBOR data.
 * @param[in]  buffer_size      Size of the output buffer.
 * @param[out] encoded_len      Length of encoded data.
 *
 * @returns 0 If the operation was successful.
 *	    Otherwise, a (negative) error code is returned.
 * @retval -EINVAL if parameters are invalid.
 * @retval -ENOMEM if buffer is too small.
 *
 */
int encode_config_reported_to_cbor(uint32_t update_interval,
				   uint32_t sample_interval,
				   uint8_t *buffer,
				   size_t buffer_size,
				   size_t *encoded_len);
