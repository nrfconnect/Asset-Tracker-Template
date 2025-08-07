/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _STORAGE_BACKEND_H_
#define _STORAGE_BACKEND_H_

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Storage backend interface.
 *
 * This structure provides function pointers that define the interface
 * for different storage backends (RAM, internal flash, external flash, SD card).
 */
struct storage_backend {
	/**
	 * @brief Initialize the storage backend.
	 *
	 * @return 0 on success, negative errno on failure
	 */
	int (*init)(void);

	/**
	 * @brief Store data in the backend.
	 *
	 * @param type Storage data type to store
	 * @param data Pointer to the data to store
	 * @param size Size of the data in bytes
	 * @return 0 on success, negative errno on failure
	 */
	int (*store)(const struct storage_data *type, const void *data, size_t size);

	/**
	 * @brief Peek at data from the backend without removing it.
	 *
	 * @param type Storage data type to peek at
	 * @param data Buffer to store the peeked data (can be NULL to just get size)
	 * @param size Size of the buffer
	 * @return Number of bytes that would be read on success, negative errno on failure
	 */
	int (*peek)(const struct storage_data *type, void *data, size_t size);

	/**
	 * @brief Retrieve data from the backend.
	 *
	 * @param type Storage data type to retrieve
	 * @param data Buffer to store the retrieved data
	 * @param size Size of the buffer
	 * @return Number of bytes read on success, negative errno on failure
	 */
	int (*retrieve)(const struct storage_data *type, void *data, size_t size);

	/**
	 * @brief Get number of records of a specific type.
	 *
	 * @param type Storage data type to count
	 * @return Number of records, negative errno on failure
	 */
	int (*count)(const struct storage_data *type);

	/**
	 * @brief Clear all stored data.
	 *
	 * @return 0 on success, negative errno on failure
	 */
	int (*clear)(void);
};

/* Get the active storage backend based on Kconfig selection */
const struct storage_backend *storage_backend_get(void);

#ifdef __cplusplus
}
#endif

#endif /* _STORAGE_BACKEND_H_ */
