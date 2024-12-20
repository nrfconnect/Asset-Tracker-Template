/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zephyr_gas_sensor_dummy

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#include "gas_sensor.h"
#include "bme68x_iaq.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(gas_sensor_dummy, CONFIG_SENSOR_LOG_LEVEL);

static int gas_sensor_dummy_sample_fetch(const struct device *dev,
				      enum sensor_channel chan)
{
	return 0;
}

static int gas_sensor_dummy_channel_get(const struct device *dev,
				     enum sensor_channel chan,
				     struct sensor_value *val)
{
	struct gas_sensor_dummy_data const *data = dev->data;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch"
	switch (chan) {
	case SENSOR_CHAN_AMBIENT_TEMP:
		return sensor_value_from_float(val, data->temperature);
	case SENSOR_CHAN_PRESS:
		return sensor_value_from_float(val, data->pressure);
	case SENSOR_CHAN_HUMIDITY:
		return sensor_value_from_float(val, data->humidity);
	case SENSOR_CHAN_IAQ:
		val->val1 = data->iaq;
		val->val2 = 0;
		break;
	case SENSOR_CHAN_CO2:
		val->val1 = data->co2;
		val->val2 = 0;
		break;
	case SENSOR_CHAN_VOC:
		val->val1 = data->voc;
		val->val2 = 0;
		break;
	default:
		return -ENOTSUP;
	}
#pragma GCC diagnostic pop
	return 0;
}

static const struct sensor_driver_api gas_sensor_dummy_api = {
	.sample_fetch = &gas_sensor_dummy_sample_fetch,
	.channel_get = &gas_sensor_dummy_channel_get,
};

static int gas_sensor_dummy_init(const struct device *dev)
{
	return 0;
}

#define EXAMPLE_SENSOR_INIT(i)						       \
	static struct gas_sensor_dummy_data gas_sensor_dummy_data_##i;	       \
									       \
									       \
	DEVICE_DT_INST_DEFINE(i, gas_sensor_dummy_init, NULL,		       \
			      &gas_sensor_dummy_data_##i,			       \
			      NULL, POST_KERNEL,	       \
			      CONFIG_SENSOR_INIT_PRIORITY, &gas_sensor_dummy_api);

DT_INST_FOREACH_STATUS_OKAY(EXAMPLE_SENSOR_INIT)
