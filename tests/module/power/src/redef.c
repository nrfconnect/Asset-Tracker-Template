/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

static int sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(chan);

	return 0;
}

static int channel_get(const struct device *dev, enum sensor_channel chan,
		      struct sensor_value *val)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(chan);
	ARG_UNUSED(val);

	return 0;
}

static int attr_get(const struct device *dev, enum sensor_channel chan,
		    enum sensor_attribute attr, struct sensor_value *val)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(chan);
	ARG_UNUSED(attr);

	/* Report zero, e.g. VBUS not present. Without this, sensor_attr_get() returns -ENOSYS. */
	val->val1 = 0;
	val->val2 = 0;

	return 0;
}

static const struct sensor_driver_api dummy_api = {
	.sample_fetch = &sample_fetch,
	.channel_get = &channel_get,
	.attr_get = &attr_get,
};

struct device_state state = {
	.initialized = true,
	.init_res = 0U,
};

struct device mock_charger_device = {
	.name = "mock_npm1300_charger",
	.state = &state,
	.api = &dummy_api
};
