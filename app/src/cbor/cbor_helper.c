/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <errno.h>
#include "device_shadow_decode.h"

int get_update_interval_from_cbor_response(const uint8_t *cbor,
					   size_t len,
					   uint32_t *interval_sec)
{
	struct config_object object = { 0 };
	size_t not_used;

	int err = cbor_decode_config_object(cbor, len, &object, &not_used);

	if (err) {
		return -EFAULT;
	}

	*interval_sec = object.update_interval;

	return 0;
}
