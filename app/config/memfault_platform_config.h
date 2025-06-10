/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

/*
 * Platform overrides for the default configuration settings in the
 * memfault-firmware-sdk. Default configuration settings can be found in
 * "<NCS folder>/modules/lib/memfault-firmware-sdk/components/include/memfault/default_config.h"
 */


#define MEMFAULT_EVENT_STORAGE_READ_BATCHING_ENABLED 1
#define MEMFAULT_EVENT_STORAGE_READ_BATCHING_MAX_BYTES 1024
