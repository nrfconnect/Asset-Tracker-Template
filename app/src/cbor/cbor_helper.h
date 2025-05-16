/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/types.h>

#define CLOUD_COMMAND_NAME_PROVISION "provision_request"
#define CLOUD_COMMAND_NAME_MAX_LENGTH 32

struct cloud_command {
	char name[CLOUD_COMMAND_NAME_MAX_LENGTH];
	uint32_t id;
};

/**
 * @brief Get the device shadow parameters from a CBOR buffer.
 *
 * @param[in]  cbor         The CBOR buffer.
 * @param[in]  len          The length of the CBOR buffer.
 * @param[out] interval_sec The update interval in seconds.
 * @param[out] cmd	    Pointer to a cloud_command structure that will be filled with the
 *			    command name and ID.
 *
 * @returns 0 If the operation was successful.
 *	    Otherwise, a (negative) error code is returned.
 * @retval -EFAULT if the CBOR buffer is invalid.
 *
 */
int get_update_parameters_from_cbor_response(const uint8_t *cbor,
					     size_t len,
					     uint32_t *interval_sec,
					     struct cloud_command *cmd);
