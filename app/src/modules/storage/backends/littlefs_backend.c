/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <math.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>

#include "storage.h"
#include "storage_backend.h"
#include "storage_data_types.h"

#define MAX_PATH_LEN     CONFIG_APP_STORAGE_LITTLEFS_MAX_PATH_LEN
#define RECORDS_PER_TYPE CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE

LOG_MODULE_REGISTER(lfs_backend, CONFIG_APP_STORAGE_LOG_LEVEL);

struct storage_file_header {
	uint32_t read_offset;
	uint32_t write_offset;
};

#define LFS_NODE DT_NODELABEL(lfs1)

/*
 * @brief Declare mountpoint based on DTS node if available, otherwise use fixed partition
 */
#if DT_NODE_EXISTS(LFS_NODE)
FS_FSTAB_DECLARE_ENTRY(LFS_NODE);
#else
/* Fallback to PM if DTS not available */
FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_config);
static struct fs_mount_t lfs_storage_mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &lfs_config,
	.storage_dev = (void *)FIXED_PARTITION_ID(littlefs_storage),
	.mnt_point = "/att_storage",
};
#endif

static struct fs_mount_t *mountpoint =
#if DT_NODE_EXISTS(LFS_NODE)
	&FS_FSTAB_ENTRY(LFS_NODE)
#else
	&lfs_storage_mnt
#endif
	;

/*
 * @brief Mount LittleFS filesystem
 *
 * Checks if automount is enabled in DTS, and mounts the filesystem if not.
 *
 * @param mp Mountpoint to mount
 * @return int 0 on success, negative errno on failure
 */
static int littlefs_mount(struct fs_mount_t *mp)
{

#if !DT_NODE_EXISTS(LFS_NODE) || !(FSTAB_ENTRY_DT_MOUNT_FLAGS(LFS_NODE) & FS_MOUNT_FLAG_AUTOMOUNT)
	int ret;

	ret = fs_mount(mp);
	if (ret < 0) {
		LOG_ERR("Failed to mount id %" PRIuPTR " at %s: %d\n", (uintptr_t)mp->storage_dev,
			mp->mnt_point, ret);

		return ret;
	}

	LOG_INF("%s mount: %d\n", mp->mnt_point, ret);
#else
	LOG_INF("%s automounted\n", mp->mnt_point);
#endif

	return 0;
}

/*
 * @brief Verify that the LittleFS partition is large enough to hold all data types
 *
 * Calculates the necessary number of blocks based on the registered storage data types
 * and their sizes, and compares it to the available blocks in the partition.
 * Asserts if the partition is too small.
 *
 * @note This function assumes that each data type uses the maximum number of records
 *      defined by RECORDS_PER_TYPE. If the actual usage is lower, the partition size
 *      requirement will be less.
 */
static void verify_partition_size(void)
{
	struct fs_statvfs stat;
	int necessary_blocks = 0;
	int ret;

	ret = fs_statvfs(mountpoint->mnt_point, &stat);
	if (ret < 0) {
		LOG_ERR("Failed to get filesystem stats: %d", ret);

		return;
	}

	LOG_DBG("Filesystem stats for %s: block size = %lu ; total blocks = %lu",
		mountpoint->mnt_point, stat.f_frsize, stat.f_blocks);

	STRUCT_SECTION_FOREACH(storage_data, type) {

		size_t max_file_size = type->data_size * RECORDS_PER_TYPE;
		size_t block_size = stat.f_frsize;

		necessary_blocks += (int)ceil((double)max_file_size / block_size);
	}
	/* 2 metadata blocks + CoW block */
	necessary_blocks += 2 + 1;

	__ASSERT(necessary_blocks <= stat.f_blocks,
		 "LittleFS partition too small. Need at least %d blocks, partition has %lu",
		 necessary_blocks, stat.f_blocks);

	LOG_INF("LittleFS partition size verified: need %d blocks, have %lu blocks",
		necessary_blocks, stat.f_blocks);
}

/*
 * @brief Create a data file path string
 *
 * @param type Storage data type
 * @param out_path Output buffer for the path string
 * @return int Number of characters written on success, negative value on error
 */
static int create_storage_file_path(const struct storage_data *type, int file_index, char *out_path)
{
	int ret = 0;

	ret = snprintk(out_path, MAX_PATH_LEN, "%s/%s_%d.bin", mountpoint->mnt_point, type->name,
		       file_index);

	if (ret < 0 || ret >= MAX_PATH_LEN) {
		LOG_ERR("Failed to create file path for %s, index %d", type->name, file_index);

		return -ENAMETOOLONG;
	}

	return ret;
}

/*
 * @brief Create a header file path object
 *
 * @param type Storage data type
 * @param out_path Output buffer for the path string
 * @return int Number of characters written on success, negative value on error
 */
static int create_storage_header_file_path(const struct storage_data *type, char *out_path)
{
	int ret = 0;

	ret = snprintk(out_path, MAX_PATH_LEN, "%s/%s.header", mountpoint->mnt_point, type->name);

	if (ret < 0 || ret >= MAX_PATH_LEN) {
		LOG_ERR("Failed to create header file path for %s", type->name);

		return -ENAMETOOLONG;
	}

	return ret;
}

/*
 * @brief Read storage file header
 *
 * Reads the storage file header for the given storage data type.
 *
 * @param type Storage data type
 * @param header Output header structure
 * @return int 0 on success, negative errno on failure
 */
static int read_storage_file_header(const struct storage_data *type,
				    struct storage_file_header *header)
{
	int ret;
	char file_path[MAX_PATH_LEN];
	struct fs_file_t file;

	ret = create_storage_header_file_path(type, file_path);
	if (ret < 0) {
		return ret;
	}

	fs_file_t_init(&file);

	ret = fs_open(&file, file_path, FS_O_READ);
	if (ret < 0) {
		LOG_ERR("Failed to open header file %s: %d", file_path, ret);

		return ret;
	}

	ret = fs_seek(&file, 0, FS_SEEK_SET);
	if (ret < 0) {
		fs_close(&file);

		return ret;
	}

	ret = (int)fs_read(&file, header, sizeof(*header));
	if (ret < 0) {
		fs_close(&file);

		return ret;
	}

	ret = fs_close(&file);
	if (ret < 0) {
		LOG_ERR("Failed to close header file %s: %d", file_path, ret);

		return ret;
	}

	return 0;
}

/*
 * @brief Write storage file header
 *
 * Writes the storage file header for the given storage data type.
 *
 * @param type Storage data type
 * @param header Header structure to write
 * @return int 0 on success, negative errno on failure
 */
static int write_storage_file_header(const struct storage_data *type,
				     const struct storage_file_header *header)
{
	int ret;
	char file_path[MAX_PATH_LEN];
	struct fs_file_t file;

	ret = create_storage_header_file_path(type, file_path);
	if (ret < 0) {
		return ret;
	}

	fs_file_t_init(&file);

	ret = fs_open(&file, file_path, FS_O_RDWR | FS_O_CREATE);
	if (ret < 0) {
		LOG_ERR("Failed to open header file %s: %d", file_path, ret);

		return ret;
	}

	ret = fs_seek(&file, 0, FS_SEEK_SET);
	if (ret < 0) {
		LOG_ERR("Failed to seek in header file %s: %d", file_path, ret);
		fs_close(&file);

		return ret;
	}

	ret = (int)fs_write(&file, header, sizeof(*header));
	if (ret < 0) {
		fs_close(&file);

		return ret;
	}

	ret = fs_close(&file);
	if (ret < 0) {
		LOG_ERR("Failed to close header file %s: %d", file_path, ret);

		return ret;
	}

	return 0;
}

/*
 * @brief Get the entries per block object
 *
 * Determines how many entries of the given storage data type can fit in a single
 * filesystem block.
 *
 * @param type Storage data type
 * @param entries_per_block Pointer to store the number of entries per block
 * @return int 0 on success, negative errno on failure
 */
static int get_entries_per_block(const struct storage_data *type, size_t *entries_per_block)
{

	struct fs_statvfs stat;
	int ret;

	ret = fs_statvfs(mountpoint->mnt_point, &stat);
	if (ret < 0) {
		LOG_ERR("Failed to get block size: %d", ret);

		return ret;
	}

	*entries_per_block = stat.f_frsize / type->data_size;
	if (*entries_per_block == 0) {
		LOG_ERR("Data size %zu exceeds block size %d", type->data_size, (int)stat.f_frsize);

		return -EFBIG;
	}

	return 0;
}

/*
 * @brief Get the file index for a given entry index
 *
 * @param entries_per_block Number of entries per block
 * @param index Entry index
 * @return int File index
 */
static int get_file_index(size_t entries_per_block, uint32_t index)
{
	return index / entries_per_block;
}

/*
 * @brief Get the offset index within a block for a given entry index
 *
 * @param entries_per_block Number of entries per block
 * @param index Entry index
 * @return int Offset index within the block
 */
static int get_entry_offset_index(size_t entries_per_block, uint32_t index)
{
	return index % entries_per_block;
}

/*
 * @brief Initialize header files for all storage data types
 *
 * Checks if header files exist for each registered storage data type,
 * and creates them with initial values if they do not.
 *
 * @return int
 */
static int init_header_files(void)
{
	int ret;

	STRUCT_SECTION_FOREACH(storage_data, type) {

		char header_file_path[MAX_PATH_LEN];
		struct fs_dirent entry_stat;
		struct storage_file_header header = {
			.read_offset = 0,
			.write_offset = 0,
		};

		ret = create_storage_header_file_path(type, header_file_path);
		if (ret < 0) {
			return ret;
		}

		/* Check if header file exists */
		ret = fs_stat(header_file_path, &entry_stat);
		if (ret == 0) {
			/* Header file exists, nothing to do */
			LOG_DBG("Header file %s already exists", header_file_path);
			continue;
		}

		/* Create and initialize header file */
		ret = write_storage_file_header(type, &header);
		if (ret < 0) {
			LOG_ERR("Failed to initialize header file %s: %d", header_file_path, ret);

			return ret;
		}

		LOG_DBG("Initialized header file %s", header_file_path);
	}

	return 0;
}

/*
 * @brief Initialize LittleFS storage backend
 *
 * Mounts the LittleFS filesystem, verifies that the partition size is sufficient,
 * and initializes header files for all storage data types,
 *
 * @return int 0 on success, negative errno on failure
 */
static int lfs_storage_init(void)
{
	int num_types;
	int ret;

	/* Ensure we don't exceed configured maximum */
	STRUCT_SECTION_COUNT(storage_data, &num_types)
	__ASSERT(num_types <= CONFIG_APP_STORAGE_MAX_TYPES,
		 "Too many storage types registered (%d). "
		 "Increase CONFIG_APP_STORAGE_MAX_TYPES.",
		 num_types);

	ret = littlefs_mount(mountpoint);
	if (ret < 0) {
		LOG_ERR("LittleFS mount failed: %d", ret);
		SEND_FATAL_ERROR();

		return ret;
	}

	LOG_DBG("LittleFS storage backend mounted at %s with %d data types", mountpoint->mnt_point,
		num_types);

	verify_partition_size();

	ret = init_header_files();
	if (ret < 0) {
		LOG_ERR("Failed to initialize header files: %d", ret);
		SEND_FATAL_ERROR();

		return ret;
	}

	return 0;
}

/*
 * @brief Store data in LittleFS storage backend
 *
 * Stores the given data for the specified storage data type, updating the storage
 * file header accordingly.
 *
 * @param type Storage data type
 * @param data Pointer to data to store
 * @param size Size of data to store
 * @return int 0 on success, negative errno on failure
 */
static int lfs_storage_store(const struct storage_data *type, const void *data, size_t size)
{
	char file_path[MAX_PATH_LEN];
	struct fs_file_t file;
	struct storage_file_header header;
	size_t write_pos;
	size_t entries_per_block;
	int was_full;
	int wrapped_index;
	int file_index;
	int entry_offset_index;
	int ret;

	__ASSERT(type != NULL, "Storage type is NULL");
	__ASSERT(data != NULL, "Data pointer is NULL");
	__ASSERT(size == type->data_size, "Data size mismatch: expected %zu, got %zu",
		 type->data_size, size);

	/* Read current header */
	ret = read_storage_file_header(type, &header);
	if (ret < 0) {
		LOG_ERR("Failed to read storage file header: %d", ret);
		SEND_FATAL_ERROR();

		return ret;
	}

	ret = get_entries_per_block(type, &entries_per_block);
	if (ret < 0) {
		return ret;
	}

	wrapped_index = header.write_offset % RECORDS_PER_TYPE;
	file_index = get_file_index(entries_per_block, wrapped_index);
	entry_offset_index = get_entry_offset_index(entries_per_block, wrapped_index);
	was_full = ((header.write_offset - header.read_offset) >= RECORDS_PER_TYPE);
	write_pos = (entry_offset_index % RECORDS_PER_TYPE) * type->data_size;

	/* Open storage file */
	ret = create_storage_file_path(type, file_index, file_path);
	if (ret < 0) {
		return ret;
	}

	fs_file_t_init(&file);

	ret = fs_open(&file, file_path, FS_O_WRITE | FS_O_CREATE);
	if (ret < 0) {
		LOG_ERR("Failed to open %s: %d", file_path, ret);

		return ret;
	}
	LOG_DBG("Storing data in file %s at offset %zu (write_offset=%u, read_offset=%u)",
		file_path, write_pos, header.write_offset, header.read_offset);

	/* Move to write position with wrap-around */
	ret = fs_seek(&file, write_pos, FS_SEEK_SET);
	if (ret < 0) {
		LOG_ERR("Failed to move to write position: %d", ret);
		fs_close(&file);

		return ret;
	}

	ret = (int)fs_write(&file, data, size);
	if (ret < 0) {
		LOG_ERR("Failed to write data: %d", ret);
		fs_close(&file);

		return ret;
	}

	ret = fs_close(&file);
	if (ret < 0) {
		LOG_ERR("Failed to close file after writing: %d", ret);

		return ret;
	}

	/* Update header */
	header.write_offset += 1;
	if (was_full) {
		header.read_offset += 1;
		LOG_WRN("Storage full for type %s, overwriting oldest data", type->name);
	}

	ret = write_storage_file_header(type, &header);
	if (ret < 0) {
		LOG_ERR("Failed to update storage file header: %d", ret);

		return ret;
	}

	return 0;
}

/*
 * @brief Read data entry from LittleFS storage backend
 *
 * Internal helper that reads the next data entry for the specified storage data type.
 * Optionally updates the read offset if update_offset is true.
 *
 * @param type Storage data type
 * @param data Pointer to buffer to store read data
 * @param size Size of buffer
 * @param update_offset If true, increment read_offset after successful read
 * @return int Number of bytes read on success, negative errno on failure
 */
static int read_data_entry(const struct storage_data *type, void *data, size_t size,
			   bool update_offset)
{
	char file_path[MAX_PATH_LEN];
	struct fs_file_t file;
	struct storage_file_header header;
	uint8_t temp_buffer[STORAGE_MAX_DATA_SIZE];
	size_t read_pos;
	size_t entries_per_block;
	int read_bytes;
	int wrapped_index;
	int file_index;
	int entry_offset_index;
	int ret;

	__ASSERT(type != NULL, "Storage type is NULL");

	/* If data is not null, size must be at least type->data_size */
	__ASSERT(data != NULL ? size >= type->data_size : true,
		 "Data size mismatch: expected at least %zu, got %zu", type->data_size, size);
	__ASSERT(type->data_size <= STORAGE_MAX_DATA_SIZE,
		 "Type data size %zu exceeds max supported %d", type->data_size,
		 STORAGE_MAX_DATA_SIZE);

	/* Read current header */
	ret = read_storage_file_header(type, &header);
	if (ret < 0) {
		LOG_ERR("Failed to read storage file header: %d", ret);
		SEND_FATAL_ERROR();

		return ret;
	}

	if (header.read_offset == header.write_offset) {
		LOG_DBG("No new entries to read for %s", type->name);

		return -EAGAIN;
	}

	ret = get_entries_per_block(type, &entries_per_block);
	if (ret < 0) {
		return ret;
	}

	wrapped_index = header.read_offset % RECORDS_PER_TYPE;
	file_index = get_file_index(entries_per_block, wrapped_index);
	entry_offset_index = get_entry_offset_index(entries_per_block, wrapped_index);
	read_pos = (entry_offset_index % RECORDS_PER_TYPE) * type->data_size;

	/* Open storage file */
	ret = create_storage_file_path(type, file_index, file_path);
	if (ret < 0) {
		return ret;
	}

	fs_file_t_init(&file);

	ret = fs_open(&file, file_path, FS_O_READ);
	if (ret < 0) {
		LOG_ERR("Failed to open %s: %d", file_path, ret);

		return ret;
	}

	LOG_DBG("%s data in file %s at offset %zu (write_offset=%u, read_offset=%u)",
		update_offset ? "Reading" : "Peeking", file_path, read_pos, header.write_offset,
		header.read_offset);

	/* Move to read position with wrap-around */
	ret = fs_seek(&file, read_pos, FS_SEEK_SET);
	if (ret < 0) {
		LOG_ERR("Failed to move to read position: %d", ret);
		fs_close(&file);

		return ret;
	}

	read_bytes = fs_read(&file, data != NULL ? data : temp_buffer, type->data_size);
	if (read_bytes < 0) {
		LOG_ERR("Failed to read data: %d", read_bytes);
		fs_close(&file);

		return read_bytes;
	}

	ret = fs_close(&file);
	if (ret < 0) {
		LOG_ERR("Failed to close file after reading: %d", ret);

		return ret;
	}

	/* Update header if requested */
	if (update_offset) {
		header.read_offset += 1;

		ret = write_storage_file_header(type, &header);
		if (ret < 0) {
			LOG_ERR("Failed to update storage file header: %d", ret);

			return ret;
		}
	}

	return read_bytes;
}

/*
 * @brief Peek data from LittleFS storage backend
 *
 * Peeks at the next data entry for the specified storage data type without
 * updating the read offset.
 *
 * @param type Storage data type
 * @param data Pointer to buffer to store peeked data
 * @param size Size of buffer
 * @return int Number of bytes read on success, negative errno on failure
 */
static int lfs_storage_peek(const struct storage_data *type, void *data, size_t size)
{
	return read_data_entry(type, data, size, false);
}

/*
 * @brief Retrieve data from LittleFS storage backend
 *
 * Retrieves the next data entry for the specified storage data type, updating
 * the read offset accordingly.
 *
 * @param type Storage data type
 * @param data Pointer to buffer to store retrieved data
 * @param size Size of buffer
 * @return int Number of bytes read on success, negative errno on failure
 */
static int lfs_storage_retrieve(const struct storage_data *type, void *data, size_t size)
{
	return read_data_entry(type, data, size, true);
}

/*
 * @brief Get the number of stored records for a given storage data type
 *
 * Reads the storage file header and calculates the number of stored records
 *
 * @param type Storage data type
 * @return int Number of records, negative errno on failure
 */
static int lfs_storage_records_count(const struct storage_data *type)
{
	struct storage_file_header header;
	int count;
	int ret;

	__ASSERT(type != NULL, "Storage type is NULL");

	/* Read current header */
	ret = read_storage_file_header(type, &header);
	if (ret < 0) {
		LOG_ERR("Failed to read storage file header: %d", ret);
		SEND_FATAL_ERROR();

		return ret;
	}

	count = header.write_offset - header.read_offset;

	return count;
}

/*
 * @brief Clear all stored data in LittleFS storage backend
 *
 * Deletes all data and header files for all storage data types, and
 * re-initializes the header files, effectively clearing all stored data.
 *
 * @return int 0 on success, negative errno on failure
 */
static int lfs_storage_clear(void)
{
	struct fs_dir_t dir;
	struct fs_dirent entry;
	char file_path[MAX_PATH_LEN];
	int ret;

	fs_dir_t_init(&dir);

	ret = fs_opendir(&dir, mountpoint->mnt_point);
	if (ret < 0) {
		LOG_ERR("Failed to open directory %s: %d", mountpoint->mnt_point, ret);

		return ret;
	}

	/* Delete all files in the directory */
	while (true) {
		ret = fs_readdir(&dir, &entry);
		if (ret < 0) {
			LOG_ERR("Failed to read directory: %d", ret);
			fs_closedir(&dir);

			return ret;
		}

		/* End of directory */
		if (entry.name[0] == 0) {
			break;
		}

		/* Skip directories and only process files */
		if (entry.type == FS_DIR_ENTRY_FILE) {
			ret = snprintk(file_path, sizeof(file_path), "%s/%s", mountpoint->mnt_point,
				 entry.name);

			if (ret < 0 || ret >= sizeof(file_path)) {
				LOG_ERR("Failed to create file path for %s", entry.name);
				fs_closedir(&dir);

				return -ENAMETOOLONG;
			}

			ret = fs_unlink(file_path);
			if (ret < 0) {
				LOG_ERR("Failed to delete file %s: %d", file_path, ret);
				fs_closedir(&dir);

				return ret;
			}

			LOG_DBG("Deleted file: %s", file_path);
		}
	}

	fs_closedir(&dir);

	/* Re-initialize header files */
	ret = init_header_files();
	if (ret < 0) {
		LOG_ERR("Failed to re-initialize header files: %d", ret);

		return ret;
	}

	LOG_INF("Storage cleared successfully");

	return 0;
}
/*
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

/*
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
