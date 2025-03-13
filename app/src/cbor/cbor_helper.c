/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(main, CONFIG_APP_LOG_LEVEL);

int get_update_interval_from_cbor_response(const uint8_t *cbor,
					   size_t len,
					   uint64_t *interval_sec)
{
	*interval_sec = 1800;
	return 0;
}
