/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/types.h>

int get_update_interval_from_cbor_response(const uint8_t *cbor,
					   size_t len,
					   uint64_t *interval_sec);
