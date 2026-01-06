/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>

#include "storage.h"
#include "storage_backend.h"
#include "storage_data_types.h"

#define MAX_PATH_LEN 255

// LOG_MODULE_DECLARE(storage, CONFIG_APP_STORAGE_LOG_LEVEL);
LOG_MODULE_REGISTER(lfs_backend, CONFIG_APP_STORAGE_LOG_LEVEL);

struct storage_file_header {
	uint32_t read_offset;
	uint32_t write_offset;
};

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(storage);
static struct fs_mount_t mountpoint = {
	.type = FS_LITTLEFS,
	.fs_data = &storage,
	.storage_dev = (void *)FIXED_PARTITION_ID(littlefs_storage),
	.mnt_point = "/storage_data",
};

#define RECORDS_PER_TYPE CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE

static int littlefs_mount(struct fs_mount_t *mp)
{
	int rc;

	rc = fs_mount(mp);
	if (rc < 0) {
		LOG_PRINTK("Failed to mount id %" PRIuPTR " at %s: %d\n",
			   (uintptr_t)mp->storage_dev, mp->mnt_point, rc);
		return rc;
	}
	LOG_PRINTK("%s mount: %d\n", mp->mnt_point, rc);
	return 0;
}

/**
 * @brief Help function to create storage file path
 *
 * This function generates the file path for storing data of a specific type by concatenating
 * the mount point with the type name and a .bin extension.
 * file path format: "<mount_point>/<type_name>.bin"
 *
 * @param type Storage data type
 * @param out_path Output buffer to hold the generated file path
 * @param max_len Maximum length of the output buffer
 */
static void create_storage_file_path(const struct storage_data *type, char *out_path, size_t max_len)
{
	snprintf(out_path, max_len, "%s/%s.bin", mountpoint.mnt_point, type->name);
}

/**
 * @brief Initialize storage file with header
 *
 * Creates a new storage file with an initialized header if it does not already exist.
 * The header contains read and write offsets and the maximum number of entries.
 *
 * @param file_path File name (path) of the storage file
 * @return int 0 on success, negative errno on failure
 */
static int init_storage_file(const char *file_path)
{
	struct fs_file_t file;
	struct storage_file_header header;
	struct fs_dirent entry_stat;
	int rc;

	/* Check if file already exists by trying to read its status */
	rc = fs_stat(file_path, &entry_stat);
	if (rc == 0) {
		/* File exists, nothing to do (preserves existing header) */
		LOG_DBG("Storage file %s already exists", file_path);
		return 0;
	}

	/* File doesn't exist, create it and initialize header */
	fs_file_t_init(&file);
	rc = fs_open(&file, file_path, FS_O_CREATE | FS_O_RDWR);
	if (rc < 0) {
		LOG_ERR("Failed to open %s: %d", file_path, rc);
		return rc;
	}

	header.read_offset = 0;
	header.write_offset = 0;

	rc = fs_write(&file, &header, sizeof(header));
	if (rc < 0) {
		LOG_ERR("Failed to write header %s: %d", file_path, rc);
		fs_close(&file);
		return rc;
	}
	fs_close(&file);
	LOG_DBG("Initialized storage file %s", file_path);
	return 0;
}

/**
 * @brief Read storage file header
 *
 * Reads the header of a storage file to retrieve read and write offsets and max entries.
 *
 * @param file Pointer to the opened storage file
 * @param header Pointer to the storage file header structure to be filled
 * @return int 0 on success, negative errno on failure
 */
static int read_storage_file_header(struct fs_file_t *file, struct storage_file_header *header)
{
	int rc;

	rc = fs_seek(file, 0, FS_SEEK_SET);
	if (rc < 0) {
		LOG_ERR("Failed to move to header: %d", rc);
		return rc;
	}

	rc = fs_read(file, header, sizeof(*header));
	if (rc < 0) {
		LOG_ERR("Failed to read header: %d", rc);
		return rc;
	}
	return 0;
}

/**
 * @brief Update storage file header
 *
 * Updates the header of a storage file with new read and write offsets.
 *
 * @param file Pointer to the opened storage file
 * @param header Pointer to the storage file header structure to be written
 * @return int 0 on success, negative errno on failure
 */
static int update_storage_file_header(struct fs_file_t *file, struct storage_file_header *header)
{
	int rc;

	rc = fs_seek(file, 0, FS_SEEK_SET);
	if (rc < 0) {
		LOG_ERR("Failed to move to header: %d", rc);
		return rc;
	}

	rc = fs_write(file, header, sizeof(*header));
	if (rc < 0) {
		LOG_ERR("Failed to write header: %d", rc);
		return rc;
	}
	return 0;
}

/**
 * @brief Initialize LittleFS storage backend
 *
 * Mounts the LittleFS filesystem and initializes storage files for each data type.
 *
 * @return int 0 on success, negative errno on failure
 */
static int lfs_storage_init(void)
{
	int num_types;
	int rc;

	rc = littlefs_mount(&mountpoint);
	if (rc < 0) {
		LOG_ERR("LittleFS mount failed: %d", rc);
		return rc;
	}

	/* Ensure we don't exceed configured maximum */
	STRUCT_SECTION_COUNT(storage_data, &num_types);
	__ASSERT(num_types <= CONFIG_APP_STORAGE_MAX_TYPES,
		 "Too many storage types registered (%d). "
		 "Increase CONFIG_APP_STORAGE_MAX_TYPES.",
		 num_types);
	LOG_DBG("LittleFS storage backend mounted at %s with %d data types",
		mountpoint.mnt_point, num_types);

	STRUCT_SECTION_FOREACH(storage_data, t) {
		char file_path[MAX_PATH_LEN];

		create_storage_file_path(t, file_path, sizeof(file_path));
		rc = init_storage_file(file_path);
		if (rc < 0) {
			LOG_ERR("Failed to initialize storage file for type %s: %d", t->name, rc);
			return rc;
		}
	}
	return 0;
}

/**
 * @brief Store data in LittleFS storage backend
 *
 * Stores data of a specific type into its corresponding storage file.
 * If the storage file is full, the oldest data will be overwritten.
 *
 * @param type Type of data to store
 * @param data Pointer to the data to store
 * @param size Size of the data to store
 * @return int 0 on success, negative errno on failure
 */
static int lfs_storage_store(const struct storage_data *type, const void *data, size_t size)
{
	char file_path[MAX_PATH_LEN];
	struct fs_file_t file;
	struct storage_file_header header;
	uint32_t write_pos;
	int was_full;
	int rc;

	if (!type || !data || size != type->data_size) {
		return -EINVAL;
	}

	/* Open storage file */
	create_storage_file_path(type, file_path, sizeof(file_path));
	fs_file_t_init(&file);
	rc = fs_open(&file, file_path, FS_O_RDWR);
	if (rc < 0) {
		LOG_ERR("Failed to open %s: %d", file_path, rc);
		return rc;
	}

	/* Read current header */
	rc = read_storage_file_header(&file, &header);
	if (rc < 0) {
		fs_close(&file);
		return rc;
	}

	was_full = ((header.write_offset - header.read_offset) >= RECORDS_PER_TYPE);
	write_pos = sizeof(header) + (header.write_offset % RECORDS_PER_TYPE) * type->data_size;
	rc = fs_seek(&file, write_pos, FS_SEEK_SET);
	if (rc < 0) {
		LOG_ERR("Failed to move to write position: %d", rc);
		fs_close(&file);
		return rc;
	}

	/* Write data to storage file */
	rc = fs_write(&file, data, size);
	if (rc < 0) {
		LOG_ERR("Failed to write data: %d", rc);
		fs_close(&file);
		return rc;
	}

	/* Update header */
	header.write_offset++;
	if (was_full) {
		/* Drop oldest when full to overwrite */
		header.read_offset++;
		LOG_WRN("Storage file %s full, overwriting oldest data", file_path);
	}
	rc = update_storage_file_header(&file, &header);
	if (rc < 0) {
		fs_close(&file);
		return rc;
	}
	fs_close(&file);
	return 0;
}

/**
 * @brief Peek data from LittleFS storage backend without removing it
 *
 * Returns the size of the next item without copying data (if data is NULL)
 * or copies the data if data buffer is provided.
 *
 * @param type Storage data type to peek at
 * @param data Pointer where the peeked data will be stored (can be NULL for size-only)
 * @param size Size of the data buffer in bytes
 * @return int Number of bytes that have been read (would be read if data is NULL) on success,
 *             -EAGAIN if no data available, negative errno on failure
 */
static int lfs_storage_peek(const struct storage_data *type, void *data, size_t size)
{
	char file_path[MAX_PATH_LEN];
	struct fs_file_t file;
	struct storage_file_header header;
	uint32_t read_pos;
	int rc;

	/* If data is NULL, use temporary buffer to verify data exists */
	uint8_t temp_buffer[type->data_size];
	void *read_buffer = (data != NULL) ? data : temp_buffer;

	if (data != NULL && size < type->data_size) {
		return -EINVAL;
	}

	if (type == NULL) {
		LOG_ERR("Storage type is NULL");
		return -EINVAL;
	}

	/* Open storage file */
	create_storage_file_path(type, file_path, sizeof(file_path));
	fs_file_t_init(&file);
	rc = fs_open(&file, file_path, FS_O_RDWR);
	if (rc < 0) {
		LOG_ERR("Failed to open %s: %d", file_path, rc);
		return rc;
	}

	/* Read current header */
	rc = read_storage_file_header(&file, &header);
	if (rc < 0) {
		LOG_ERR("Failed to read storage file header: %d", rc);
		fs_close(&file);
		return rc;
	}
	if (header.read_offset == header.write_offset) {
		LOG_DBG("No new entries to peek in %s", file_path);
		fs_close(&file);
		return -EAGAIN;
	}

	/* Move to read position with wrap-around */
	read_pos = sizeof(header) + (header.read_offset % RECORDS_PER_TYPE) * type->data_size;
	rc = fs_seek(&file, read_pos, FS_SEEK_SET);
	if (rc < 0) {
		LOG_ERR("Failed to move to read position: %d", rc);
		fs_close(&file);
		return rc;
	}

	/* Read data from storage file without advancing read offset */
	rc = fs_read(&file, read_buffer, type->data_size);
	if (rc < 0) {
		LOG_ERR("Failed to read data: %d", rc);
		fs_close(&file);
		return rc;
	}
	fs_close(&file);
	return rc;
}

/**
 * @brief Retrieve data from LittleFS storage backend
 *
 * Retrieves the oldest stored data for the given data type from its storage file.
 *
 * @param type Storage data type to retrieve data for
 * @param data Pointer where the retrieved data will be stored
 * @param size Size of the data buffer in bytes
 * @return int Number of bytes read on success, -EAGAIN if no data available,
 *             negative errno on failure
 */
static int lfs_storage_retrieve(const struct storage_data *type, void *data, size_t size)
{
	char file_path[MAX_PATH_LEN];
	struct fs_file_t file;
	struct storage_file_header header;
	uint32_t read_pos;
	int rc, read_bytes;

	if (type == NULL || data == NULL || size < type->data_size) {
		return -EINVAL;
	}

	/* Open storage file */
	create_storage_file_path(type, file_path, sizeof(file_path));
	fs_file_t_init(&file);
	rc = fs_open(&file, file_path, FS_O_RDWR);
	if (rc < 0) {
		LOG_ERR("Failed to open %s: %d", file_path, rc);
		return rc;
	}
	/* Read current header */
	rc = read_storage_file_header(&file, &header);
	if (rc < 0) {
		fs_close(&file);
		return rc;
	}
	/* Empty if counters are equal */
	if (header.read_offset == header.write_offset) {
		LOG_WRN("No new entries to read in %s", file_path);
		fs_close(&file);
		return -EAGAIN;
	}
	/* Move to read position with wrap-around */
	read_pos = sizeof(header) + (header.read_offset % RECORDS_PER_TYPE) * type->data_size;
	rc = fs_seek(&file, read_pos, FS_SEEK_SET);
	if (rc < 0) {
		LOG_ERR("Failed to move to read position: %d", rc);
		fs_close(&file);
		return rc;
	}
	/* Read data from storage file */
	read_bytes = fs_read(&file, data, type->data_size);
	if (read_bytes < 0) {
		LOG_ERR("Failed to read data: %d", read_bytes);
		fs_close(&file);
		return read_bytes;
	}
	/* Update header to advance read offset */
	header.read_offset++;
	rc = update_storage_file_header(&file, &header);
	if (rc < 0) {
		fs_close(&file);
		return rc;
	}
	fs_close(&file);
	return read_bytes;
}

/**
 * @brief Get number of records of a specific type in LittleFS storage backend
 *
 * @param type Storage data type to count records for
 * @return int Number of records on success, negative errno on failure
 */
static int lfs_storage_records_count(const struct storage_data *type)
{
	char file_path[MAX_PATH_LEN];
	struct fs_file_t file;
	struct storage_file_header header;
	uint32_t count;
	int rc;

	if (!type) {
		return -EINVAL;
	}
	/* Open storage file */
	create_storage_file_path(type, file_path, sizeof(file_path));
	fs_file_t_init(&file);
	rc = fs_open(&file, file_path, FS_O_RDWR);
	if (rc < 0) {
		LOG_ERR("Failed to open %s: %d", file_path, rc);
		return rc;
	}
	/* Read current header */
	rc = read_storage_file_header(&file, &header);
	if (rc < 0) {
		fs_close(&file);
		return rc;
	}
	/* Calculate count */
	count = header.write_offset - header.read_offset;
	if (count > RECORDS_PER_TYPE) {
		count = RECORDS_PER_TYPE;
	}
	LOG_DBG("Storage file %s has %d records", file_path, count);

	fs_close(&file);
	return (int)count;
}

/**
 * @brief Clear all stored data in LittleFS storage backend
 *
 * Resets the read and write offsets in the headers of all storage files,
 * effectively clearing the stored data.
 *
 * @return int 0 on success, negative errno on failure
 */
static int lfs_storage_clear(void)
{
	STRUCT_SECTION_FOREACH(storage_data, type) {
		char file_path[MAX_PATH_LEN];
		struct storage_file_header header;
		struct fs_file_t file;
		int rc;

		/* Open storage file */
		create_storage_file_path(type, file_path, sizeof(file_path));
		fs_file_t_init(&file);
		rc = fs_open(&file, file_path, FS_O_RDWR);
		if (rc < 0) {
			LOG_ERR("Failed to open %s: %d", file_path, rc);
			return rc;
		}
		/* Reset header offsets */
		header.read_offset = 0;
		header.write_offset = 0;
		rc = update_storage_file_header(&file, &header);
		if (rc < 0) {
			fs_close(&file);
			return rc;
		}
		fs_close(&file);
		LOG_DBG("Cleared storage file %s", file_path);
	}
	return 0;
}
/**
 * @brief LittleFS storage backend interface
 *
 * Implementation of the storage_backend interface for LittleFS-based storage.
 * Provides functions for initializing the backend, storing and retrieving
 * data, counting stored records, and clearing all data.
 */
static const struct storage_backend lfs_backend = {
	.init = lfs_storage_init,
	.store = lfs_storage_store,
	.peek = lfs_storage_peek,
	.retrieve = lfs_storage_retrieve,
	.count = lfs_storage_records_count,
	.clear = lfs_storage_clear,
};

/**
 * @brief Get the LittleFS storage backend interface
 *
 * Makes the LittleFS storage backend available to the storage module.
 *
 * @return Pointer to the LittleFS storage backend interface
 */
const struct storage_backend *storage_backend_get(void)
{
	return &lfs_backend;
}
