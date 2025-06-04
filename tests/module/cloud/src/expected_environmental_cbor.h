/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef EXPECTED_ENVIRONMENTAL_CBOR_H
#define EXPECTED_ENVIRONMENTAL_CBOR_H

#include <zephyr/types.h>

#include "environmental.h"

extern const struct environmental_msg env_samples[33];
extern const size_t env_samples_size;
extern const uint8_t expected_environmental_cbor_20[];
extern const size_t expected_environmental_cbor_20_len;
extern const uint8_t expected_environmental_cbor_33[];
extern const size_t expected_environmental_cbor_33_len;
extern const uint8_t expected_environmental_single_cbor[];
extern const size_t expected_environmental_single_cbor_len;
extern const uint8_t expected_environmental_grouped_cbor_20[];
extern const size_t expected_environmental_grouped_cbor_20_len;
extern const uint8_t expected_environmental_cbor_13[];
extern const size_t expected_environmental_cbor_13_len;
extern const uint8_t expected_environmental_cbor_5[];
extern const size_t expected_environmental_cbor_5_len;

#endif /* EXPECTED_ENVIRONMENTAL_CBOR_H */
