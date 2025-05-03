/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

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
BUILD_ASSERT(STORAGE_RAM_SIZE <= (CONFIG_APP_STORAGE_RAM_LIMIT_KB * 1024),
	    "Storage RAM usage exceeds configured limit. Adjust MAX_TYPES, "
	    "MAX_RECORDS_PER_TYPE, RECORD_SIZE, or increase RAM_LIMIT_KB.");

struct storage_area_header {
	const size_t record_size;
	const size_t buf_size;
	size_t num_records_in_use;
	uint8_t *buf;
};

/* Note that the first three members have the same offset for all types, meaning they can always
 * be accessed by casting an area pointer to a pointer to struct storage_area_header. */
#define RAM_STORAGE_AREA_ADD(_name, _c, _m, _data_type, _cf, _ef)			\
	struct _name##_storage_area {							\
		const size_t record_size;						\
		const size_t buf_size;							\
		size_t num_records_in_use;						\
		uint8_t buf[RECORDS_PER_TYPE * sizeof(_data_type)];			\
	} _name;

#define RAM_STORAGE_ARE_SIZES_SET(_name, _c, _m, _data_type, _cf, _ef)			\
	._name = {									\
		.record_size = sizeof(_data_type),					\
		.buf_size = RECORDS_PER_TYPE * sizeof(_data_type),			\
	},

#define RAM_STORAGE_AREA_PTR_SET(_name, _c, _m, _dt, _cf, _ef)				\
	&ctx.areas._name,

/* RAM backend context */
struct ram_backend_ctx {
	/* Storage areas for each data type */
	struct storage_areas {
		/* Add a storage are for each of the registered data sources.
		* Example:
		*	For the power module, still will expand to
		*
		*	struct power_storage_area {						\
		*		uint8_t power_storage_buf[RECORDS_PER_TYPE * sizeof(double)];	\
		*		size_t power_num_records;					\
		*	} power;
		*/
		DATA_SOURCE_LIST(RAM_STORAGE_AREA_ADD)

		/* Array with pointers to the storage areas */
		void *area_ptrs[CONFIG_APP_STORAGE_MAX_TYPES];
	} areas;

	/* Number of data types registered */
	int num_registered_types;
};

static struct ram_backend_ctx ctx = {
	.areas = {
		DATA_SOURCE_LIST(RAM_STORAGE_ARE_SIZES_SET)

		.area_ptrs = {
			DATA_SOURCE_LIST(RAM_STORAGE_AREA_PTR_SET)
		}
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

static void *get_storage_area_ptr(size_t idx)
{
	return ctx.areas.area_ptrs[idx];
}

static int ram_init(void)
{
	memset(&ctx, 0, sizeof(ctx));

	/* Count registered types */
	STRUCT_SECTION_FOREACH(storage_data_type, t) {
		ctx.num_registered_types++;
	}

	LOG_DBG("RAM backend initialized with %d types, using %d bytes of RAM",
		ctx.num_registered_types, STORAGE_RAM_SIZE);

	/* Ensure we don't exceed configured maximum */
	__ASSERT(ctx.num_registered_types <= CONFIG_APP_STORAGE_MAX_TYPES,
		 "Too many storage types registered (%d). "
		 "Increase CONFIG_APP_STORAGE_MAX_TYPES.",
		 ctx.num_registered_types);

	return 0;
}

static int ram_store(const struct storage_data_type *type, const void *data, size_t size)
{
	struct storage_area_header *area;
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

	area = get_storage_area_ptr(idx);
	if (area->num_records_in_use >= RECORDS_PER_TYPE) {
		return -ENOSPC;
	}

	/* Store the data */
	memcpy(area->records[area->num_records].data, data, size);

	area->records[area->num_records].size = size;
	area->num_records++;

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

	area = get_storage_area_ptr(idx);

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
