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

#define RECORDS_PER_TYPE CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE

/* Calculate total RAM usage for storage areas */
#define STORAGE_RAM_SIZE (CONFIG_APP_STORAGE_MAX_TYPES * \
			 RECORDS_PER_TYPE * \
			 CONFIG_APP_STORAGE_RECORD_SIZE)

/* Ensure RAM usage doesn't exceed configured limit */
BUILD_ASSERT(STORAGE_RAM_SIZE <= KB(CONFIG_APP_STORAGE_RAM_LIMIT_KB),
	    "Storage RAM usage exceeds configured limit. Adjust MAX_TYPES, "
	    "MAX_RECORDS_PER_TYPE, RECORD_SIZE, or increase RAM_LIMIT_KB.");

/* Note that the first three members have the same offset for all types, meaning they can always
 * be accessed by casting an area pointer to a pointer to struct storage_area_header. */
#define RAM_RING_BUF_ADD(_name, _c, _m, _data_type, _cf, _ef)				\
	RING_BUF_DECLARE(_CONCAT(_name, _ring_buf), (_data_type * RECORDS_PER_TYPE));

#define RAM_RING_BUF_PTR_SET(_name, _c, _m, _dt, _cf, _ef)				\
	&_CONCAT(_name, _ring_buf),

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
		DATA_SOURCE_LIST(RAM_RING_BUF_PTR_SET)
	}
};

/**
 * @brief Get the index for a storage data type
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

static struct ring_buf *get_ring_buf_ptr(size_t idx)
{
	__ASSERT_NO_MSG(idx <= (ctx.num_registered_types - 1))

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

static int ram_store(const struct storage_data_type *type, const void *data, size_t size)
{
	int ret;
	struct ring_buf *ring_buf;
	int idx;

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

	if (ring_buf_space_get(ring_buf) <= type->data_size) {
		LOG_DBG("Old data will be overwritten");
	}

	/* Store the data */
	ret = ring_buf_put(ring_buf, data, size);
	__ASSERT_NO_MSG(ret == size);

	(void)ret;

	return 0;
}

static int ram_retrieve(const struct storage_data_type *type, void *data, size_t size)
{
	struct storage_area *area;
	int bytes_read;
	int idx;

	if (!type || !data) {
		return -EINVAL;
	}

	idx = get_type_index(type);
	if (idx < 0) {
		return -EINVAL;
	}

	area = get_ring_buf_ptr(idx);

	if (area->num_records == 0) {
		return -ENOENT;
	}

	/* Get the oldest record (FIFO) */
	if (size < area->records[0].size) {
		return -ENOSPC;
	}

	/* Copy the data */
	memcpy(data, area->records[0].data, area->records[0].size);

	bytes_read = area->records[0].size;

	/* Move remaining records forward */
	if (area->num_records > 1) {
		memmove(&area->records[0], &area->records[1],
			(area->num_records - 1) * sizeof(struct storage_record));
	}

	area->num_records--;

	return bytes_read;
}

static int ram_count(const struct storage_data_type *type)
{
	int idx;

	if (!type) {
		return -EINVAL;
	}

	idx = get_type_index(type);
	if (idx < 0) {
		return -EINVAL;
	}



	return ctx.areas[idx].num_records;
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
	.count = ram_count,
	.clear = ram_clear,
};

/* Make the RAM backend available */
const struct storage_backend *storage_backend_get(void)
{
	return &ram_backend;
}
