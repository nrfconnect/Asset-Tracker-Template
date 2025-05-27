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

#ifdef __cplusplus
extern "C" {
#endif

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
