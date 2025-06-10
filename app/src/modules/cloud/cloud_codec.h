/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _CLOUD_CODEC_H_
#define _CLOUD_CODEC_H_

#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stdint.h>

#include "storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Defines that can be used to estimate the size of encoded environmental data.
 * An element is encoded using encode_sensor_message() which produces a CBOR map with
 * temperature, humidity, and pressure fields.
 */
#define CLOUD_CODEC_ENV_ELEMENT_CBOR_SIZE 		76
#define CLOUD_CODEC_BATTERY_ELEMENT_CBOR_SIZE		26

#define CLOUD_CODEC_CBOR_ARRAY_HEADER_SIZE		2

/**
 * @brief Encode an array of environmental data samples into CBOR format using direct pointers
 *
 * This function encodes multiple environmental samples directly from storage chunks
 * without copying the data to a temporary buffer. It maintains a zero-copy approach
 * from storage through encoding.
 *
 * @param[out] payload Buffer to store the encoded CBOR data
 * @param[in] payload_len Size of the payload buffer
 * @param[out] payload_out_len Actual length of encoded data
 * @param[in] chunks Array of pointers to storage chunks containing environmental data
 * @param[in] num_chunks Number of chunks in the array
 *
 * @return 0 on success, negative errno code on failure
 * @retval 0 Success
 * @retval -EINVAL Invalid parameters (null pointers or zero chunks)
 * @retval -ENOMEM Buffer too small for the encoded data
 * @retval -EIO Encoding operation failed
 */
int encode_data_chunk_array(uint8_t *payload, size_t payload_len,
                                      size_t *payload_out_len,
                                      struct storage_data_chunk **chunks,
                                      size_t num_chunks);

/**
 * @brief Encode an array of environmental data samples into CBOR format
 *
 * @param[out] payload Buffer to store the encoded CBOR data
 * @param[in] payload_len Size of the payload buffer
 * @param[out] payload_out_len Actual length of encoded data
 * @param[in] env_samples Array of environmental samples to encode
 * @param[in] num_samples Number of samples in the array
 *
 * @return 0 on success, negative errno code on failure
 * @retval 0 Success
 * @retval -EINVAL Invalid parameters (null pointers or zero samples)
 * @retval -ENOMEM Buffer too small for the encoded data
 * @retval -EIO Encoding operation failed
 */
int encode_environmental_data_array(uint8_t *payload, size_t payload_len,
			            size_t *payload_out_len,
				    const struct environmental_msg *env_samples,
				    size_t num_samples);

/**
 * @brief Encode a single environmental data sample into CBOR format
 *
 * @param[out] payload Buffer to store the encoded CBOR data
 * @param[in] payload_len Size of the payload buffer
 * @param[out] payload_out_len Actual length of encoded data
 * @param[in] sample Environmental sample to encode
 * @param[in] timestamp Timestamp in milliseconds, use 0 to get current time
 *
 * @return 0 on success, negative errno code on failure
 * @retval 0 Success
 * @retval -EINVAL Invalid parameters (null pointers)
 * @retval -ENOMEM Buffer too small for the encoded data
 * @retval -EIO Encoding operation failed
 */
int encode_environmental_sample(uint8_t *payload, size_t payload_len,
			        size_t *payload_out_len,
			        const struct environmental_msg *sample,
			        int64_t timestamp);

#ifdef __cplusplus
}
#endif

#endif /* _CLOUD_CODEC_H_ */
