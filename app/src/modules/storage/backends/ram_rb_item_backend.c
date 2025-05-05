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

/* Calculate total RAM usage for storage areas */
#define STORAGE_RAM_SIZE	(CONFIG_APP_STORAGE_MAX_TYPES *				\
				 RECORDS_PER_TYPE *					\
				 CONFIG_APP_STORAGE_RECORD_SIZE)

/* Ensure RAM usage doesn't exceed configured limit */
BUILD_ASSERT(STORAGE_RAM_SIZE <= KB(CONFIG_APP_STORAGE_RAM_LIMIT_KB),
	    "Storage RAM usage exceeds configured limit. Adjust MAX_TYPES, "
	    "MAX_RECORDS_PER_TYPE, RECORD_SIZE, or increase RAM_LIMIT_KB.");


#define RAM_RING_BUF_ADD(_name, _c, _m, _data_type, _cf, _ef)				\
	RING_BUF_ITEM_DECLARE(_name ## _ring_buf,				\
			      (RING_BUF_ITEM_SIZEOF(_data_type) * RECORDS_PER_TYPE));

#define RAM_RING_BUF_PTR(_name, _c, _m, _dt, _cf, _ef)					\
	&(_name ## _ring_buf),

/* Declare ring buffers for each data type */
DATA_SOURCE_LIST(RAM_RING_BUF_ADD)

/* RAM backend context */
struct ram_backend_ctx {
	/* Array with pointers to the ring buffers.
	 * We use an array of pointers instead of nmed pointers to make it flexible
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
	}
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
static int get_type_index(const struct storage_data_type *type)
{
	size_t idx = 0;

	STRUCT_SECTION_FOREACH(storage_data_type, t) {
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
 * from the context structure. The index is must be retrieved by the
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

static int ram_init(void)
{
	/* Count registered types */
	STRUCT_SECTION_FOREACH(storage_data_type, t) {
		ctx.num_registered_types++;
	}

	/* Ensure we don't exceed configured maximum */
	__ASSERT(ctx.num_registered_types <= CONFIG_APP_STORAGE_MAX_TYPES,
		 "Too many storage types registered (%d). "
		 "Increase CONFIG_APP_STORAGE_MAX_TYPES.",
		 ctx.num_registered_types);

	LOG_DBG("RAM backend initialized with %d types, using %d bytes of RAM",
		ctx.num_registered_types, STORAGE_RAM_SIZE);

	return 0;
}

static int ram_store(const struct storage_data_type *type, void *data, size_t size)
{
	int err;
	struct ring_buf *ring_buf;
	int idx;
	uint16_t unused_type = 0;
	uint8_t unused_value = 0;

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

	if (ring_buf_item_space_get(ring_buf) <= type->data_size) {
		uint16_t unused_type;
		uint8_t unused_value;
		uint8_t unused_size = type->data_size;

		LOG_DBG("Old data will be overwritten");

		/* Remove the oldest record in the ring buffer */
		(void)ring_buf_item_get(ring_buf, &unused_type, &unused_value, NULL, &unused_size);
	}

	/* Store the data */
	/* TODO: Use type and value? */
	err = ring_buf_item_put(ring_buf, unused_type, unused_value, data,
				RING_BUF_ITEM_SIZEOF(size));

	/* We should never fail here, as we checked space above */
	__ASSERT_NO_MSG(err == 0);

	(void)err;

	return 0;
}

static int ram_retrieve(const struct storage_data_type *type, void *data, size_t size)
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

	return bytes_read;
}

static int ram_records_count(const struct storage_data_type *type)
{
	int idx;
	struct ring_buf *ring_buf;
	size_t capacity;
	size_t words_used;
	size_t words_free;
	size_t item_count;
	/* 1 word for the header of a ring buffer item */
	const size_t ring_buf_header_size = 1;

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
	capacity = ring_buf_capacity_get(ring_buf);
	words_used = (capacity - words_free) / sizeof(uint32_t);

	/* Calculate the number of items in the ring buffer */
	item_count = words_used / (ring_buf_header_size + RING_BUF_ITEM_SIZEOF(type->data_size));

	return item_count;
}

static int ram_clear(void)
{
	memset(&ctx, 0, sizeof(ctx));

	return 0;
}

/* RAM backend interface */
static const struct storage_backend ram_backend = {
    .init = ram_init,
    .store = ram_store,
    .retrieve = ram_retrieve,
    .count = ram_records_count,
    .clear = ram_clear,
};

/* Make the RAM backend available */
const struct storage_backend *storage_backend_get(void)
{
	return &ram_backend;
}
