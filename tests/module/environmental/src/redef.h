/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef REDEF_H_
#define REDEF_H_

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

extern struct device mock_sensor_device;

/* When set to a channel other than SENSOR_CHAN_ALL, the mock channel_get() implementation
 * will fail (return -EIO) for that channel only. Reset to SENSOR_CHAN_ALL between tests.
 */
extern enum sensor_channel mock_channel_get_fail_channel;

#undef DEVICE_DT_GET
#define DEVICE_DT_GET(node_id) &mock_sensor_device

#endif /* REDEF_ */
