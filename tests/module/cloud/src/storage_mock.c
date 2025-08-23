/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/logging/log.h>

/* Include the actual headers instead of redefining */
#include "../../../app/src/modules/storage/storage.h"
#include "../../../app/src/modules/location/location.h"
#include "../../../app/src/modules/environmental/environmental.h"

LOG_MODULE_REGISTER(storage_mock, CONFIG_LOG_DEFAULT_LEVEL);

/* Mock storage channels */
ZBUS_CHAN_DEFINE(STORAGE_CHAN,
		 struct storage_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.type = STORAGE_MODE_PASSTHROUGH)
);

ZBUS_CHAN_DEFINE(STORAGE_DATA_CHAN,
		 struct storage_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.type = STORAGE_DATA)
);

/* Note: LOCATION_CHAN is already defined in cloud_module_test.c */

/* Note: location functions are provided by the actual headers, no need to mock them */

/* Mock storage batch read function */
int storage_batch_read(struct storage_data_item *out_item, k_timeout_t timeout)
{
	LOG_DBG("Mock storage_batch_read called");

	if (!out_item) {
		return -EINVAL;
	}

	/* Return no data available to simulate empty storage */
	return -EAGAIN;
}