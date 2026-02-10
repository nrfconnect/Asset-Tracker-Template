/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef TEST_SAMPLES_H
#define TEST_SAMPLES_H

#include <zephyr/types.h>
#include <modem/location.h>

#include "power.h"
#include "environmental.h"
#include "location.h"

/* Battery samples */
extern const double battery_samples[100];
extern const size_t battery_samples_size;

/* Environmental samples */
extern const struct environmental_msg env_samples[100];
extern const size_t env_samples_size;

/* Location samples */
extern const struct location_msg location_samples[100];
extern const size_t location_samples_size;

#endif /* TEST_SAMPLES_H */
