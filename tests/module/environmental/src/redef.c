/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

static int sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(chan);

	return 0;
}

enum sensor_channel mock_channel_get_fail_channel = SENSOR_CHAN_ALL;

static int channel_get(const struct device *dev, enum sensor_channel chan,
		      struct sensor_value *val)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(val);

	if (chan == mock_channel_get_fail_channel) {
		return -EIO;
	}

	return 0;
}

static const struct sensor_driver_api dummy_api = {
	.sample_fetch = &sample_fetch,
	.channel_get = &channel_get,
};

struct device_state state = {
	.initialized = true,
	.init_res = 0U,
};

struct device mock_sensor_device = {
	.name = "mock_sensor",
	.state = &state,
	.api = &dummy_api
};
