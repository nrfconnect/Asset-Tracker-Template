/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _STORAGE_DATA_TYPES_H_
#define _STORAGE_DATA_TYPES_H_

#include <zephyr/zbus/zbus.h>
#include <zephyr/kernel.h>


#include "power.h"
#include "environmental.h"
#include "location.h"

/**
 * @brief List of data sources that can be stored by the storage module
 *
 * This macro defines a list of data sources that the storage module will handle.
 * Each entry in the list specifies:
 * - name: Identifier for this data type (e.g., battery)
 * - channel: zbus channel to listen on (e.g., POWER_CHAN)
 * - msg_type: Type of messages on the channel (e.g., struct power_msg)
 * - data_type: Type of data to store (e.g., double for battery percentage)
 * - check_fn: Function to filter messages (e.g., battery_check)
 * - extract_fn: Function to extract data (e.g., battery_extract)
 *
 * The list uses IF_ENABLED to conditionally include data sources based on Kconfig:
 * - CONFIG_APP_POWER enables battery data storage
 * - CONFIG_APP_LOCATION enables location data storage
 * - CONFIG_APP_ENVIRONMENTAL enables environmental data storage
 *
 * This macro is used in three ways:
 *
 * 1. With STORAGE_DATA_TYPE to register each data type:
 *    DATA_SOURCE_LIST(STORAGE_DATA_TYPE) expands to:
 *    STORAGE_DATA_TYPE(battery, POWER_CHAN, struct power_msg, double,
 *                     battery_check, battery_extract)
 *    for each enabled module
 *
 * 2. With ADD_OBSERVERS to add storage_subscriber to each channel:
 *    DATA_SOURCE_LIST(ADD_OBSERVERS) expands to:
 *    ZBUS_CHAN_ADD_OBS(POWER_CHAN, storage_subscriber, 0)
 *    for each enabled module
 *
 * 3. With MAX_MSG_SIZE_FROM_LIST to calculate buffer sizes:
 *    Used to ensure message buffers are large enough for all message types
 *
 * @param X Macro to apply to each entry in the list. Will be called as:
 *          X(name, channel, msg_type, data_type, check_fn, extract_fn)
 */
#define DATA_SOURCE_LIST(X)										   \
	IF_ENABLED(CONFIG_APP_POWER, (X(battery, POWER_CHAN,	struct power_msg, double, battery_check, battery_extract)))	   \
	IF_ENABLED(CONFIG_APP_LOCATION, (X(location, LOCATION_CHAN, enum location_msg_type, enum location_msg_type, location_check, location_extract)))  \
	IF_ENABLED(CONFIG_APP_ENVIRONMENTAL, (X(environmental, ENVIRONMENTAL_CHAN, struct environmental_msg, struct environmental_msg, environmental_check, environmental_extract)))


/* Structure to define a storage data type */
struct storage_data_type {
	/* Name of the data type */
	const char *name;

	/* Channel to subscribe to */
	const struct zbus_channel *chan;

	/* Size of data to store. This must be less than or equal to
	 * CONFIG_APP_STORAGE_RECORD_SIZE.
	 */
	size_t data_size;

	/* Size of the required storage for the data type.
	 * Must be equal to or less than .data_size * CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE.
	 */
	size_t storage_size;

	/**
	 * @brief Function to determine if a message should be stored
	 *
	 * This function examines a message from the channel and decides if its data
	 * should be stored. For example, for environmental data, you might only want
	 * to store actual sensor readings and ignore status messages.
	 * It will be called by the storage module when a message is received on the channel
	 * in the .chan field of the structure.
	 *
	 * @param msg Pointer to the message from the channel. Will be cast to the
	 *            appropriate message type internally.
	 *
	 * @retval true if the message contains data that should be stored
	 * @retval false if the message should be ignored
	 */
	bool (*should_store)(const void *msg);

	/**
	 * @brief Function to extract data from a message into storage format
	 *
	 * This function extracts the relevant data from a message and converts it
	 * into the format that should be stored. For example, from a power message
	 * you might extract just the battery percentage value.
	 *
	 * @param msg Pointer to the message to extract data from. Will be cast to
	 *            the appropriate message type internally.
	 * @param data Pointer to where the extracted data should be stored. Will
	 *             be cast to the appropriate data type internally.
	 */
	void (*extract_data)(const void *msg, void *data);
};

// TODO: Generate extract and check functions automatically?
/**
 * @brief Register a storage data type.
 *
 * This macro registers a new data type for storage, defining how to handle messages
 * from a specific zbus channel and how to store their data. It creates the necessary
 * helper functions and registers the type in an iterable section.
 *
 * Example usage:
 * @code
 * STORAGE_DATA_TYPE(battery,               // Name for this storage type
 *                   POWER_CHAN,            // Channel to listen on
 *                   struct power_msg,      // Type of messages on the channel
 *                   double,                // Type of data to store
 *                   battery_should_store,  // Function to check if message should be stored
 *                   battery_extract_data); // Function to extract data from message
 * @endcode
 *
 * @param _name Name to identify this storage type
 * @param _chan zbus channel to listen on for data
 * @param _msg_type Type of messages received on the channel
 * @param _data_type Type of data to store
 * @param _check_fn Function that returns true if a message should be stored
 * @param _extract_fn Function that extracts data from a message into storage format
 */
#define STORAGE_DATA_TYPE(_name, _chan, _msg_type, _data_type, _check_fn, _extract_fn)		\
	BUILD_ASSERT(sizeof(_data_type) <= CONFIG_APP_STORAGE_RECORD_SIZE,			\
		    "Data type too large for storage record");					\
												\
	extern bool _name##_check(const _msg_type *msg);					\
	extern void _name##_extract(const _msg_type *msg, _data_type *data);			\
												\
	static bool _name##_should_store(const void *msg)					\
	{											\
		const _msg_type *m = (_msg_type *)msg;						\
		return _name##_check(m);							\
	}											\
												\
	static void _name##_extract_data(const void *msg, void *data)				\
	{											\
		const _msg_type *m = (_msg_type *)msg;						\
		_extract_fn(m, (_data_type *)data);						\
	}											\
												\
	STRUCT_SECTION_ITERABLE(storage_data_type, _name##_storage_type) = {			\
		.name = #_name,									\
		.chan = &_chan,									\
		.data_size = sizeof(_data_type),						\
		.storage_size = (sizeof(_data_type) * CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE),	\
		.should_store = _name##_should_store,						\
		.extract_data = _name##_extract_data,						\
	};

#endif /* _STORAGE_DATA_TYPES_H_ */
