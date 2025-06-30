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
 * @param[in]  cbor         The CBOR buffer.
 * @param[in]  len          The length of the CBOR buffer.
 * @param[out] interval_sec Update interval in seconds.
 * @param[out] command_type Cloud command type.
 *
 * @returns 0 If the operation was successful.
 *	    Otherwise, a (negative) error code is returned.
 * @retval -EFAULT if the CBOR buffer is invalid.
 *
 */
int get_parameters_from_cbor_response(const uint8_t *cbor,
				      size_t len,
				      uint32_t *interval_sec,
				      uint32_t *command_type);
