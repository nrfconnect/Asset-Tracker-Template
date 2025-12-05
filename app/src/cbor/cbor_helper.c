/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <errno.h>
#include <string.h>
#include <zcbor_encode.h>

#include "cbor_helper.h"
#include "device_shadow_types.h"
#include "device_shadow_decode.h"
#include "device_shadow_encode.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(cbor_helper, CONFIG_APP_LOG_LEVEL);

int decode_shadow_parameters_from_cbor(const uint8_t *cbor,
				       size_t len,
				       struct config_params *config,
				       uint32_t *command_type,
				       uint32_t *command_id)
{
	int err;
	struct shadow_object shadow = { 0 };
	size_t decode_len = len;

	if (!cbor || !config || !command_type || len == 0) {
		LOG_ERR("Invalid input");
		return -EINVAL;
	}

	err = cbor_decode_shadow_object(cbor, decode_len, &shadow, &decode_len);
	if (err) {
		LOG_ERR("cbor_decode_shadow_object, error: %d", err);
		LOG_HEXDUMP_ERR(cbor, len, "Unexpected CBOR data");
		return -EFAULT;
	}

	if (shadow.config_present) {
		if (shadow.config.update_interval_present) {
			config->update_interval = shadow.config.update_interval.update_interval;
			LOG_DBG("Configuration: Decoded update_interval = %d seconds",
				config->update_interval);
		}

		if (shadow.config.sample_interval_present) {
			config->sample_interval = shadow.config.sample_interval.sample_interval;
			LOG_DBG("Configuration: Decoded sample_interval = %d seconds",
				config->sample_interval);
		}

		if (shadow.config.buffer_mode_present) {
			config->buffer_mode = shadow.config.buffer_mode.buffer_mode;
			config->buffer_mode_valid = true;
			LOG_DBG("Configuration: Decoded buffer_mode = %s",
				config->buffer_mode ? "enabled" : "disabled");
		}
	}

	if (shadow.command_present) {
		*command_type = shadow.command.type;
		*command_id = shadow.command.id;

		LOG_DBG("Command parameter present: type=%u, id=%u",
			*command_type, *command_id);

	}

	return 0;
}

int encode_shadow_parameters_to_cbor(const struct config_params *config,
				     uint32_t command_type,
				     uint32_t command_id,
				     uint8_t *buffer,
				     size_t buffer_size,
				     size_t *encoded_len)
{
	int err;
	struct shadow_object shadow = { 0 };
	size_t encode_len;

	if (!config || !buffer || !encoded_len || buffer_size == 0) {
		return -EINVAL;
	}

	/* Build shadow object with config section */
	if (config->update_interval > 0) {
		shadow.config_present = true;
		shadow.config.update_interval_present = true;
		shadow.config.update_interval.update_interval = config->update_interval;
	}

	if (config->sample_interval > 0) {
		shadow.config_present = true;
		shadow.config.sample_interval_present = true;
		shadow.config.sample_interval.sample_interval = config->sample_interval;
	}

	if (config->buffer_mode_valid) {
		shadow.config_present = true;
		shadow.config.buffer_mode_present = true;
		shadow.config.buffer_mode.buffer_mode = config->buffer_mode;
	}

	/* Build shadow object with command section */
	if (command_type > 0) {
		shadow.command_present = true;
		shadow.command.type = command_type;
		shadow.command.id = command_id;
	}

	/* Encode the shadow object to CBOR */
	err = cbor_encode_shadow_object(buffer, buffer_size, &shadow, &encode_len);
	if (err) {
		LOG_ERR("cbor_encode_shadow_object, error: %d", err);
		return (err == ZCBOR_ERR_NO_PAYLOAD) ? -ENOMEM : -EFAULT;
	}

	*encoded_len = encode_len;

	return 0;
}
