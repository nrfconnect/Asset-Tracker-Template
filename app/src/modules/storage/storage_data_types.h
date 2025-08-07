/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _STORAGE_DATA_TYPES_H_
#define _STORAGE_DATA_TYPES_H_

#include <zephyr/zbus/zbus.h>
#include <zephyr/kernel.h>

#include "app_common.h"
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
 * - CONFIG_APP_ENVIRONMENTAL enables environmental data storage
 * - CONFIG_APP_LOCATION enables location data storage
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
#define DATA_SOURCE_LIST(X)									\
	IF_ENABLED(CONFIG_APP_POWER,								\
		   (X(BATTERY, POWER_CHAN, struct power_msg, double,				\
		      battery_check, battery_extract)))						\
	IF_ENABLED(CONFIG_APP_ENVIRONMENTAL,							\
		   (X(ENVIRONMENTAL, ENVIRONMENTAL_CHAN,					\
		      struct environmental_msg, struct environmental_msg,			\
		      environmental_check, environmental_extract)))				\
	IF_ENABLED(CONFIG_APP_LOCATION,								\
		   (X(LOCATION, LOCATION_CHAN, struct location_msg,				\
		      struct location_msg, location_check, location_extract)))

#define STORAGE_DATA_TYPE(_name)								\
	STORAGE_TYPE_ ## _name

#define _STORAGE_DATA_TYPE_ID(_name, _chan, _msg_type, _data_type, _check_fn, _extract_fn)	\
	STORAGE_DATA_TYPE(_name),

#define _STORAGE_DATA_TYPE_MEMBER(_name, _chan, _msg_type, _data_type, _check_fn, _extract_fn)	\
	_data_type _name;


/* Calculate the maximum data size from the list of channels */
#define STORAGE_DATA_SIZE_OF_TYPE(_name, _chan, _msg_type, _data_type, _check_fn, _extract_fn)	\
	sizeof(_data_type),

#define STORAGE_MAX_DATA_SIZE_FROM_LIST(_DATA_SOURCE_LIST_LIST)	\
	MAX_N(_DATA_SOURCE_LIST_LIST(STORAGE_DATA_SIZE_OF_TYPE) 0)

/**
 * @brief Maximum size in bytes of any data type that can be stored
 *
 * This macro calculates the maximum size in bytes among all data types that can be stored
 * by the storage module. It is computed at compile time by examining all data types defined
 * in the DATA_SOURCE_LIST macro and finding the largest one.
 *
 * The calculation works as follows:
 * 1. DATA_SOURCE_LIST is expanded with STORAGE_DATA_SIZE_OF_TYPE to create a list of sizeof()
 *    expressions for each enabled data type
 * 2. MAX_N finds the maximum value among these sizes
 * 3. The result is the size of the largest data type that needs to be stored
 *
 * For example, if the enabled data types are:
 * - BATTERY: sizeof(double) = 8 bytes
 * - ENVIRONMENTAL: sizeof(struct environmental_msg) = ~24 bytes
 * - LOCATION: sizeof(struct location_msg) = ~300+ bytes (with CONFIG_LOCATION_DATA_DETAILS)
 *
 * Then STORAGE_MAX_DATA_SIZE = ~300+ bytes (the size of struct location_msg)
 *
 * The location_msg size includes the largest union member, which is struct location_data when
 * CONFIG_LOCATION_DATA_DETAILS is enabled. This structure contains:
 * - Basic location data (lat, lon, accuracy, datetime): ~40 bytes
 * - location_data_details structure with GNSS details: ~250+ bytes
 *   - Contains nrf_modem_gnss_pvt_data_frame (~250+ bytes) including:
 *     - Position, velocity, time data: ~100 bytes
 *     - Array of 12 satellite info structures: 12 * 12 = 144 bytes
 *     - Additional GNSS timing and accuracy data: ~10+ bytes
 *
 * This macro is used for:
 * - Sizing message buffers in struct storage_msg to ensure they can hold any data type
 * - Allocating temporary buffers for data processing operations (e.g., in flush_stored_data())
 *
 * The value is automatically updated when new data types are added to DATA_SOURCE_LIST
 * or when existing data types change size, ensuring the storage system remains consistent.
 */
#define STORAGE_MAX_DATA_SIZE	STORAGE_MAX_DATA_SIZE_FROM_LIST(DATA_SOURCE_LIST)

/**
 * @brief Unique identifiers for each type of data that can be stored.
 *
 * This enumeration is automatically populated by the DATA_SOURCE_LIST macro.
 * Each entry in DATA_SOURCE_LIST (e.g., BATTERY, LOCATION, ENVIRONMENTAL)
 * will result in a corresponding enumerator in this enum, prefixed with
 * "STORAGE_TYPE_". For example, if BATTERY is defined in DATA_SOURCE_LIST,
 * this enum will contain STORAGE_TYPE_BATTERY.
 */
enum storage_data_type {
	STORAGE_DATA_UNKNOWN = 0x0,

	STORAGE_DATA_ALL = 0x1,

	DATA_SOURCE_LIST(_STORAGE_DATA_TYPE_ID)

	STORAGE_DATA_TYPE_COUNT,
};

/**
 * @brief Buffer for the data type of each storage entry
 *
 * This union provides a buffer for each data type defined in DATA_SOURCE_LIST.
 * Each member is a value (not a pointer) of the corresponding data type, allowing
 * storage of the actual data for each type. For example, if BATTERY is defined in
 * DATA_SOURCE_LIST, this union will contain:
 *     double BATTERY;
 * The union is used to allocate enough space for any supported data type.
 */
union storage_data_type_buf {
	DATA_SOURCE_LIST(_STORAGE_DATA_TYPE_MEMBER)
};

/** @brief Structure to define a storage data type */
struct storage_data {
	/* Name of the data type */
	const char *name;

	/* Channel to subscribe to */
	const struct zbus_channel *chan;

	/* Type of message on the channel */
	const enum storage_data_type data_type;

	/* Size of data to store.
	 */
	size_t data_size;

	/* Pointer to the memory slab for this data type */
	struct k_mem_slab *slab;

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

/* Helper macro to create a name with a numerical value so that the linker can place the struct
 * in a predictable location based on its appearance in the input list to STORAGE_DATA_TYPE.
 */
#define _STORAGE_TYPE_NAME(_name)	CONCAT(storage_type_, __COUNTER__, _, _name)

/**
 * @brief Register a storage data type.
 *
 * This macro registers a new data type for storage, defining how to handle messages
 * from a specific zbus channel and how to store their data. It creates the necessary
 * helper functions and registers the type in an iterable section.
 *
 * Example usage:
 * @code
 * STORAGE_DATA_TYPE(battery,               - Name for this storage type
 *                   POWER_CHAN,            - Channel to listen on
 *                   struct power_msg,      - Type of messages on the channel
 *                   double,                - Type of data to store
 *                   battery_should_store,  - Function to check if message should be stored
 *                   battery_extract_data); - Function to extract data from message
 * @endcode
 *
 * @param _name Name to identify this storage type
 * @param _chan zbus channel to listen on for data
 * @param _msg_type Type of messages received on the channel
 * @param _data_type Type of data to store
 * @param _check_fn Function that returns true if a message should be stored
 * @param _extract_fn Function that extracts data from a message into storage format
 */
#define STORAGE_DATA_TYPE_ADD(_name, _chan, _msg_type, _data_type, _check_fn, _extract_fn)	\
												\
	extern bool _check_fn(const _msg_type * msg);						\
	extern void _extract_fn(const _msg_type * msg, _data_type * data);			\
												\
	static bool _name ## _should_store(const void *msg)					\
	{											\
		const _msg_type *m = (_msg_type *)msg;						\
												\
		return _check_fn(m);								\
	}											\
												\
	static void _name ## _extract_data(const void *msg, void *data)				\
	{											\
		const _msg_type *m = (_msg_type *)msg;						\
												\
		_extract_fn(m, (_data_type *)data);						\
	}											\
												\
	K_MEM_SLAB_DEFINE_STATIC(_name ## _slab,						\
				 sizeof(_data_type),						\
				 CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE, 4);			\
												\
	STRUCT_SECTION_ITERABLE(storage_data, _STORAGE_TYPE_NAME(_name)) = {			\
		.name = #_name,									\
		.chan = &_chan,									\
		.data_type = STORAGE_DATA_TYPE(_name),						\
		.data_size = sizeof(_data_type),						\
		.should_store = _name ## _should_store,						\
		.extract_data = _name ## _extract_data,						\
		.slab = &_name ## _slab,							\
	};

#endif /* _STORAGE_DATA_TYPES_H_ */
