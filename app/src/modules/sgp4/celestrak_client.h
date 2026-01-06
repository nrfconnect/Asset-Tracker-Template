/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef CELESTRAK_CLIENT_H
#define CELESTRAK_CLIENT_H

#include <zephyr/kernel.h>

/**
 * @brief Fetch TLE data for a satellite from Celestrak
 *
 * @param catnr NORAD catalog number of the satellite
 * @param buffer Buffer to store the TLE data
 * @param buffer_size Size of the buffer
 * @param bytes_written Number of bytes written to the buffer
 * @return 0 on success, negative error code on failure
 */
int celestrak_fetch_tle(const char *catnr, char *buffer, size_t buffer_size, size_t *bytes_written);

/**
 * @brief Initialize the Celestrak client
 *
 * @return 0 on success, negative error code on failure
 */
int celestrak_client_init(void);

#endif /* CELESTRAK_CLIENT_H */