/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zcbor_encode.h>
#include <zcbor_decode.h>
#include <zcbor_common.h>
#include <date_time.h>

#include "cloud.h"
#include "storage.h"

LOG_MODULE_REGISTER(cloud_codec, 4);

/* CBOR tag values from the nRF Cloud CDDL schema at
 * https://github.com/nrfconnect/sdk-nrf/tree/main/subsys/net/lib/nrf_cloud/coap/cddl
 */
#define CBOR_TAG_APP_ID 1
#define CBOR_TAG_DATA   2
#define CBOR_TAG_TS     3

/* Encode a sensor message with efficient short-circuit error handling */
static bool encode_sensor_msg_float(zcbor_state_t *state, const char *app_id_str, double value,
				    uint64_t timestamp)
{
	bool success;

	__ASSERT_NO_MSG(state != NULL);
	__ASSERT_NO_MSG(app_id_str != NULL);

	/* Chain all operations with && for immediate failure on error */
	success = zcbor_map_start_encode(state, 3) &&
			/* AppId field */
			zcbor_uint32_put(state, CBOR_TAG_APP_ID) &&
			/* strlen() is safe since app_id_str is a null-terminated string literal */
			zcbor_tstr_put_term(state, app_id_str, strlen(app_id_str)) &&

			/* Data field - encode as float */
			zcbor_uint32_put(state, CBOR_TAG_DATA) &&
			zcbor_float32_put(state, value) &&

			/* Timestamp field */
			zcbor_uint32_put(state, CBOR_TAG_TS) &&
			zcbor_uint64_put(state, timestamp) &&

			/* End the map */
			zcbor_map_end_encode(state, 3);

	if (!success) {
		LOG_ERR("Failed to encode %s message", app_id_str);
	}

	return success;
}

/* Encode an array of sensor messages directly from storage chunks */
int encode_data_chunk_array(uint8_t *payload, size_t payload_len,
                                	size_t *payload_out_len,
                                	struct storage_data_chunk **chunks,
                                	size_t num_chunks)
{
	zcbor_state_t states[4]; /* 3 levels of CBOR state nesting */
	bool success;
	size_t num_elements = num_chunks * 3; /* 3 messages per sample */

	/* Parameter validation */
	if (!payload || !payload_out_len || !chunks || num_chunks == 0) {
		return -EINVAL;
	}

	/* Initialize CBOR state with total number of elements (3 messages per sample) */
	zcbor_new_encode_state(states, ARRAY_SIZE(states), payload, payload_len, num_elements);

	/* Start array with total number of elements */
	success = zcbor_list_start_encode(states, num_elements);

	/* Encode each sample directly from the chunk data */
	for (size_t i = 0; success && i < num_chunks; i++) {
		int64_t msg_timestamp = 0;
		int err;
		struct storage_data_chunk *chunk = chunks[i];

		err = date_time_now(&msg_timestamp);
		if (err) {
			LOG_ERR("Could not get current time: %d", err);
		}

		switch (chunk->type) {
		case STORAGE_TYPE_ENVIRONMENTAL:
			success = encode_sensor_msg_float(states, "TEMP",
					chunk->data.ENVIRONMENTAL.temperature, msg_timestamp) &&
				  encode_sensor_msg_float(states, "HUMID",
					chunk->data.ENVIRONMENTAL.humidity, msg_timestamp) &&
				  encode_sensor_msg_float(states, "AIR_PRESS",
					chunk->data.ENVIRONMENTAL.pressure, msg_timestamp);
			break;
		case STORAGE_TYPE_BATTERY:
			success = encode_sensor_msg_float(states, "BATTERY",
					(double)chunk->data.BATTERY, msg_timestamp);
			break;
		default:
			LOG_ERR("Unsupported storage data type: %d", chunk->type);
			return -EINVAL;
		}
	}

	/* End the array */
	success = success && zcbor_list_end_encode(states, num_elements);

	if (!success) {
		LOG_ERR("Failed to encode environmental samples array");
		return -EIO;
	}

	/* Calculate the output length */
	*payload_out_len = states[0].payload - payload;

	return 0;
}

/* Encode an array of environmental messages */
int encode_environmental_data_array(uint8_t *payload, size_t payload_len,
				    size_t *payload_out_len,
				    const struct environmental_msg *env_samples,
				    size_t num_samples)
{
	zcbor_state_t states[4]; /* 3 levels of CBOR state nesting */
	bool success;

	/* Parameter validation */
	if (payload == NULL || payload_out_len == NULL || env_samples == NULL || num_samples == 0) {
		return -EINVAL;
	}

	/* Initialize CBOR state, element count is 0 since it is not enforced */
	zcbor_new_encode_state(states, ARRAY_SIZE(states), payload, payload_len, num_samples);

	success = zcbor_list_start_encode(states, num_samples * 3);

	/* Encode each sample if array start was successful */
	for (size_t i = 0; success && i < num_samples; i++) {
		int err;
		int64_t msg_timestamp = 0;

		err = date_time_now(&msg_timestamp);
		if (err) {
			LOG_ERR("Could not get current time: %d", err);
		}

		success = encode_sensor_msg_float(states, "TEMP",
				(double)env_samples[i].temperature, msg_timestamp) &&
			 encode_sensor_msg_float(states, "HUMID",
				(double)env_samples[i].humidity, msg_timestamp) &&
			 encode_sensor_msg_float(states, "AIR_PRESS",
				(double)env_samples[i].pressure, msg_timestamp);
	}

	/* End the array */
	success = success && zcbor_list_end_encode(states, num_samples * 3);

	if (!success) {
		return -EIO;
	}

	/* Calculate the output length */
	*payload_out_len = states[0].payload - payload;

	return 0;
}

/* Function to send a single environmental message sample */
int encode_environmental_sample(uint8_t *payload, size_t payload_len,
				size_t *payload_out_len,
				const struct environmental_msg *sample,
				uint64_t timestamp)
{
	int err;
	zcbor_state_t states[40]; /* Only need 3 levels for sensor array */
	bool success;

	/* Parameter validation */
	if (payload == NULL || payload_out_len == NULL || sample == NULL) {
		return -EINVAL;
	}

	/* Initialize CBOR state, element count is 0 since it is not enforced */
	zcbor_new_encode_state(states, ARRAY_SIZE(states), payload, payload_len, 0);

	/* Use the provided timestamp or get current time */
	if (timestamp == 0) {
		err = date_time_now(&timestamp);
		if (err) {
			LOG_ERR("Could not get current time: %d", err);
		}
	}

	success = zcbor_list_start_encode(states, 3);

	/* Chain the encoding of all three messages with && */
	success = encode_sensor_msg_float(states, "TEMP", (double)sample->temperature, timestamp) &&
		  encode_sensor_msg_float(states, "HUMID", (double)sample->humidity, timestamp) &&
		  encode_sensor_msg_float(states, "AIR_PRESS", (double)sample->pressure, timestamp);

	/* End the array */
	success = success && zcbor_list_end_encode(states, 3);

	if (!success) {
		LOG_ERR("Failed to encode environmental sample");
		return -EIO;
	}

	/* Calculate the output length */
	*payload_out_len = states[0].payload - payload;

	return 0;
}
