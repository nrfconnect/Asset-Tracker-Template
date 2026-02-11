/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/types.h>

#define CLOUD_COMMAND_TYPE_PROVISION 1

/** Device configuration parameters. */
struct config_params {
	/** Update interval in seconds. */
	uint32_t update_interval;

	/** Sample interval in seconds. */
	uint32_t sample_interval;

	/** Buffer mode flag */
	bool buffer_mode;

	/** Buffer mode validity flag */
	bool buffer_mode_valid;

	/** Storage buffer threshold amount */
	uint32_t storage_threshold;

	/** Seperate validity flag as 0 is a valid option for storage_threshold */
	bool storage_threshold_valid;
};

/**
 * @brief Decode shadow parameters from CBOR buffer.
 *
 * @param[in]  cbor         CBOR buffer.
 * @param[in]  len          CBOR buffer length.
 * @param[out] config       Decoded configuration parameters.
 * @param[out] command_type Decoded command type.
 * @param[out] command_id   Decoded command ID.
 *
 * @retval 0 Success.
 * @retval -EINVAL Invalid parameters.
 * @retval -EFAULT Invalid CBOR data.
 */
int decode_shadow_parameters_from_cbor(const uint8_t *cbor,
				       size_t len,
				       struct config_params *config,
				       uint32_t *command_type,
				       uint32_t *command_id);

/**
 * @brief Encode shadow parameters to CBOR buffer.
 *
 * @param[in]  config       Configuration parameters.
 * @param[in]  command_type Command type.
 * @param[in]  command_id   Command ID.
 * @param[out] buffer       Output buffer for encoded CBOR.
 * @param[in]  buffer_size  Output buffer size.
 * @param[out] encoded_len  Length of encoded data.
 *
 * @retval 0 Success.
 * @retval -EINVAL Invalid parameters.
 * @retval -ENOMEM Buffer too small.
 * @retval -EFAULT Encoding error.
 */
int encode_shadow_parameters_to_cbor(const struct config_params *config,
				     uint32_t command_type,
				     uint32_t command_id,
				     uint8_t *buffer,
				     size_t buffer_size,
				     size_t *encoded_len);
