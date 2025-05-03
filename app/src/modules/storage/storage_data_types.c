/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#include "storage.h"
#include "storage_data_types.h"

/**
 * @brief Register all enabled data types with the storage module
 *
 * This expands DATA_SOURCE_LIST with STORAGE_DATA_TYPE_ADD to register each enabled
 * data source. For example, with CONFIG_APP_POWER enabled, it expands to:
 *
 * STORAGE_DATA_TYPE_ADD(battery, POWER_CHAN, struct power_msg, double,
 *			 battery_check, battery_extract)
 *
 * This creates:
 * 1. Helper functions to process messages:
 *    - battery_should_store(): Filters battery messages
 *    - battery_extract_data(): Extracts battery percentage
 * 2. A storage_data struct in the iterable section:
 *    - Links the channel to its processing functions
 *    - Makes the type available to STRUCT_SECTION_FOREACH
 */
DATA_SOURCE_LIST(STORAGE_DATA_TYPE_ADD)

/* Power module storage */
#ifdef CONFIG_APP_POWER
#include "power.h"

/* Provide functions used by storage module to check and extract data */
bool battery_check(const struct power_msg *msg)
{
	return msg->type == POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE;
}

void battery_extract(const struct power_msg *msg, double *data)
{
	*data = msg->percentage;
}
#endif /* CONFIG_APP_POWER */

/* Location module storage */
#ifdef CONFIG_APP_LOCATION
#include "location.h"

/* Provide functions used by storage module to check and extract data */
bool location_check(const enum location_msg_type *msg)
{
	/* Store all location messages */
	return true;
}

void location_extract(const enum location_msg_type *msg, enum location_msg_type *data)
{
	*data = *msg;
}
#endif /* CONFIG_APP_LOCATION */

/* Environmental module storage */
#ifdef CONFIG_APP_ENVIRONMENTAL
#include "environmental.h"

/* Provide functions used by storage module to check and extract data */
bool environmental_check(const struct environmental_msg *msg)
{
	return msg->type == ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE;
}

void environmental_extract(const struct environmental_msg *msg,
			   struct environmental_msg *data)
{
	*data = *msg;
}
#endif /* CONFIG_APP_ENVIRONMENTAL */
