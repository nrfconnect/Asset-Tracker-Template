/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#include "storage.h"
#include "storage_backend.h"
#include "storage_data_types.h"

LOG_MODULE_DECLARE(storage, CONFIG_APP_STORAGE_LOG_LEVEL);

#define RECORDS_PER_TYPE	CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE
/* 1 word for the header of a ring buffer item */
#define RING_BUF_HEADER_SIZE	1

/**
 * @brief Macro to declare a ring buffer for a specific data type
 *
 * This macro is used with DATA_SOURCE_LIST to create ring buffers for each data type.
 * For each data type in DATA_SOURCE_LIST, it declares a ring buffer with:
 * - Name: <type_name>_ring_buf (e.g., battery_ring_buf)
 * - Size: Calculated to hold RECORDS_PER_TYPE items of the data type's size
 *        plus header space for each item
 *
 * @param _name Name of the data type (e.g., battery)
 * @param _c Channel parameter (unused in this macro)
 * @param _m Message type parameter (unused in this macro)
 * @param _data_type Data type to store
 * @param _cfn Check function parameter (unused in this macro)
 * @param _efn Extract function parameter (unused in this macro)
 */
#define RAM_RING_BUF_ADD(_name, _c, _m, _data_type, _cfn, _efn) 				\
	RING_BUF_ITEM_DECLARE(_name ## _ring_buf,               				\
		((RING_BUF_ITEM_SIZEOF(_data_type) + RING_BUF_HEADER_SIZE) * RECORDS_PER_TYPE));

/**
 * @brief Macro to create a pointer to a ring buffer
 *
 * This macro is used with DATA_SOURCE_LIST to create an array of pointers to
 * the ring buffers declared by RAM_RING_BUF_ADD. For each data type, it:
 * - Creates a pointer to the corresponding ring buffer
 * - Adds a comma for array initialization
 *
 * Used in the ring_buf_ptrs array initialization to create a flexible array
 * of pointers that automatically updates when DATA_SOURCE_LIST changes.
 *
 * @param _name Name of the data type (used to reference its ring buffer)
 * @param _c Channel parameter (unused in this macro)
 * @param _m Message type parameter (unused in this macro)
 * @param _dt Data type parameter (unused in this macro)
 * @param _cfn Check function parameter (unused in this macro)
 * @param _efn Extract function parameter (unused in this macro)
 */
#define RAM_RING_BUF_PTR(_name, _c, _m, _dt, _cfn, _efn)					\
	&(_name ## _ring_buf),

/* Declare ring buffers for each data type */
DATA_SOURCE_LIST(RAM_RING_BUF_ADD)

/* RAM backend context */
struct ram_backend_ctx {
	/* Array with pointers to the ring buffers.
	 * We use an array of pointers instead of named pointers to make it flexible
	 * when DATA_SOURCE_LIST is updated.
	 */
	struct ring_buf *ring_buf_ptrs[CONFIG_APP_STORAGE_MAX_TYPES];

	/* Number of data types registered */
	int num_registered_types;
};

static struct ram_backend_ctx ctx = {
	.ring_buf_ptrs = {
		/* Expands to a list of ring buffer pointers for each data type */
		DATA_SOURCE_LIST(RAM_RING_BUF_PTR)
	},
};

/**
 * @brief Get the index for a storage data type
 *
 * This function iterates through the registered storage data types that are defined
 * in the storage_data_types.h file.
 *
 * @param type Storage data type to get index for
 * @return Index into the areas array, or -1 if type not found
 */
static int get_type_index(const struct storage_data *type)
{
	size_t idx = 0;

	STRUCT_SECTION_FOREACH(storage_data, t) {
		if (t == type) {
			return idx;
		}

		idx++;
	}

	return -1;
}

/**
 * @brief Get the ring buffer pointer for a specific data type
 *
 * This function retrieves the ring buffer pointer for a specific data type
 * from the context structure. The index must be retrieved by the
 * get_type_index function.
 *
 * @param idx Index of the data type
 * @return Pointer to the ring buffer for the specified data type
 */
static struct ring_buf *get_ring_buf_ptr(size_t idx)
{
	__ASSERT_NO_MSG(idx <= (ctx.num_registered_types - 1));

	return ctx.ring_buf_ptrs[idx];
}

/**
 * @brief Initialize the RAM storage backend
 *
 * Counts the number of registered data types and ensures it doesn't exceed
 * the configured maximum. Also calculates and logs the total RAM usage.
 *
 * @return 0 on success, negative errno on failure
 */
static int ram_init(void)
{
	/* Count registered types */
	STRUCT_SECTION_FOREACH(storage_data, t) {
		int idx = get_type_index(t);
		struct ring_buf *ring_buf;
		size_t size;

		if (idx < 0) {
			LOG_ERR("Failed to get index for %s", t->name);
			return -EINVAL;
		}

		ctx.num_registered_types++;

		LOG_DBG("RAM backend initialized with %d types, using %d bytes of RAM",
			ctx.num_registered_types,
			(RING_BUF_ITEM_SIZEOF(t->data_size) + RING_BUF_HEADER_SIZE) * RECORDS_PER_TYPE);

		ring_buf = get_ring_buf_ptr(idx);
		size = ring_buf_capacity_get(ring_buf);

		LOG_DBG("Ring buffer %s initialized with size %u, item size: %u", t->name, size,
			t->data_size);
	}

	/* Ensure we don't exceed configured maximum */
	__ASSERT(ctx.num_registered_types <= CONFIG_APP_STORAGE_MAX_TYPES,
		 "Too many storage types registered (%d). "
		 "Increase CONFIG_APP_STORAGE_MAX_TYPES.",
		 ctx.num_registered_types);

	return 0;
}

/**
 * @brief Store data in the RAM backend
 *
 * Stores data in the ring buffer associated with the given data type.
 * If the ring buffer is full, the oldest data will be overwritten.
 *
 * @param type Storage data type to store data for
 * @param data Pointer to the data to store
 * @param size Size of the data in bytes
 *
 * @return 0 on success, negative errno on failure
 */
static int ram_store(const struct storage_data *type, void *data, size_t size)
{
	int err;
	struct ring_buf *ring_buf;
	int idx;
	uint16_t unused_type = 0;
	uint8_t unused_value = 0;
	uint8_t size_words = DIV_ROUND_UP(size, sizeof(uint32_t));

	if (!type || !data) {
		return -EINVAL;
	}

	if (size > CONFIG_APP_STORAGE_RECORD_SIZE) {
		return -EINVAL;
	}

	idx = get_type_index(type);
	if (idx < 0) {
		return -EINVAL;
	}

	ring_buf = get_ring_buf_ptr(idx);

	LOG_DBG("idx: %d, ring_buf: %p, data size: %u (%u), free: %u bytes",
		idx, ring_buf, size, type->data_size, ring_buf_space_get(ring_buf));

	if (ring_buf_item_space_get(ring_buf) < DIV_ROUND_UP(type->data_size, 4)) {
		uint8_t unused_size = type->data_size;

		LOG_DBG("Full buffer, old data will be overwritten");
		LOG_DBG("Capacity: %u, free space: %u",
			ring_buf_capacity_get(ring_buf),
			ring_buf_item_space_get(ring_buf));

		/* Remove the oldest record in the ring buffer */
		(void)ring_buf_item_get(ring_buf, &unused_type, &unused_value, NULL, &unused_size);
	}

	/* Store the data */
	err = ring_buf_item_put(ring_buf, unused_type, unused_value, data, size_words);

	LOG_DBG("Stored %s item, count: %u, left: %u bytes, %u items",
		type->name, storage_backend_get()->count(type),
		ring_buf_space_get(ring_buf),
		ring_buf_space_get(ring_buf) / (type->data_size + RING_BUF_HEADER_SIZE * 4));

	/* We should never fail here, as we checked space above */
	__ASSERT_NO_MSG(err == 0);

	return err;
}

/**
 * @brief Retrieve data from the RAM backend
 *
 * Retrieves the oldest stored data for the given data type from its ring buffer.
 *
 * @param type Storage data type to retrieve data for
 * @param data Pointer where the retrieved data will be stored
 * @param size Size of the data buffer in bytes
 * @return Number of bytes read on success, negative errno on failure
 */
static int ram_retrieve(const struct storage_data *type, void *data, size_t size)
{
	int err;
	size_t bytes_read;
	struct ring_buf *ring_buf;
	int idx;
	uint16_t unused_type;
	uint8_t unused_value;
	uint8_t size32 = size / sizeof(uint32_t);

	if (!type || !data) {
		return -EINVAL;
	}

	idx = get_type_index(type);
	if (idx < 0) {
		return -EINVAL;
	}

	ring_buf = get_ring_buf_ptr(idx);

	err = ring_buf_item_get(ring_buf, &unused_type, &unused_value, data, &size32);
	if (err) {
		LOG_ERR("Failed to retrieve data: %d", err);
		return err;
	}

	bytes_read = size32 * sizeof(uint32_t);

	LOG_DBG("Retrieved item in %s ring buffer, size: %u bytes, %u items left",
		type->name, size, storage_backend_get()->count(type));

	return bytes_read;
}

/**
 * @brief Count the number of records stored for a data type
 *
 * Calculates how many records are currently stored in the ring buffer
 * for the given data type.
 *
 * @param type Storage data type to count records for
 * @return Number of records on success, negative errno on failure
 */
static int ram_records_count(const struct storage_data *type)
{
	int idx;
	struct ring_buf *ring_buf;
	size_t capacity_words;
	size_t words_used;
	size_t words_free;
	size_t item_count;

	if (!type) {
		return -EINVAL;
	}

	idx = get_type_index(type);
	if (idx < 0) {
		return -EINVAL;
	}

	ring_buf = get_ring_buf_ptr(idx);

	if (ring_buf_is_empty(ring_buf)) {
		return 0;
	}

	words_free = ring_buf_item_space_get(ring_buf);
	capacity_words = ring_buf_capacity_get(ring_buf) / 4;
	words_used = capacity_words - words_free;

	/* Calculate the number of items in the ring buffer */
	item_count = words_used / (RING_BUF_HEADER_SIZE + DIV_ROUND_UP(type->data_size, 4));

	LOG_DBG("Counted %zu items in %s ring buffer", item_count, type->name);

	return item_count;
}

/**
 * @brief Clear all stored data in the RAM backend
 *
 * Resets all ring buffers to their empty state using the Zephyr ring buffer API,
 * clearing any stored data while preserving the ring buffer context structure.
 *
 * @return 0 on success
 * @retval -EINVAL if no types are registered
 */
static int ram_clear(void)
{
	size_t idx;

	if (ctx.num_registered_types == 0) {
		return -EINVAL;
	}

	/* Iterate through all registered types */
	for (idx = 0; idx < ctx.num_registered_types; idx++) {
		struct ring_buf *ring_buf = get_ring_buf_ptr(idx);

		ring_buf_reset(ring_buf);
	}

	return 0;
}

/**
 * @brief RAM storage backend interface
 *
 * Implementation of the storage_backend interface for RAM-based storage.
 * Provides functions for initializing the backend, storing and retrieving
 * data, counting stored records, and clearing all data.
 */
static const struct storage_backend ram_backend = {
	.init = ram_init,
	.store = ram_store,
	.retrieve = ram_retrieve,
	.count = ram_records_count,
	.clear = ram_clear,
};

/**
 * @brief Get the RAM storage backend interface
 *
 * Makes the RAM storage backend available to the storage module.
 *
 * @return Pointer to the RAM storage backend interface
 */
const struct storage_backend *storage_backend_get(void)
{
	return &ram_backend;
}
