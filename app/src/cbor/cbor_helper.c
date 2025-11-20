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

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(cbor_helper, CONFIG_APP_LOG_LEVEL);

int get_parameters_from_cbor_response(const uint8_t *cbor,
				      size_t len,
				      uint32_t *update_interval,
				      uint32_t *sample_interval,
				      uint32_t *command_type)
{
	int err;
	struct shadow_object shadow = { 0 };
	size_t decode_len = len;

	if (!cbor || !update_interval || !sample_interval || !command_type || len == 0) {
		return -EINVAL;
	}

	err = cbor_decode_shadow_object(cbor, decode_len, &shadow, &decode_len);
	if (err) {
		LOG_ERR("cbor_decode_shadow_object, error: %d", err);
		LOG_HEXDUMP_ERR(cbor, len, "Unexpected CBOR data");
		return -EFAULT;
	}

	if (shadow.config_present && shadow.config.update_interval_present) {
		*update_interval = shadow.config.update_interval.update_interval;
	} else {
		LOG_DBG("Update interval parameter not present");
	}

	if (shadow.config_present && shadow.config.sample_interval_present) {
		*sample_interval = shadow.config.sample_interval.sample_interval;
	} else {
		LOG_DBG("Sample interval parameter not present");
	}

	if (shadow.command_present) {
		*command_type = shadow.command.type;
		/* ID entry not used */
	} else {
		LOG_DBG("Command parameter not present");
	}

	return 0;
}

int encode_config_reported_to_cbor(uint32_t update_interval,
				   uint32_t sample_interval,
				   uint8_t *buffer,
				   size_t buffer_size,
				   size_t *encoded_len)
{
	int err;
	struct shadow_object shadow = { 0 };
	size_t encode_len;

	if (!buffer || !encoded_len || buffer_size == 0) {
		return -EINVAL;
	}

	/* Build shadow object with config section */
	shadow.config_present = true;

	if (update_interval != UINT32_MAX) {
		shadow.config.update_interval_present = true;
		shadow.config.update_interval.update_interval = update_interval;
	}

	if (sample_interval != UINT32_MAX) {
		shadow.config.sample_interval_present = true;
		shadow.config.sample_interval.sample_interval = sample_interval;
	}

	/* Encode the shadow object to CBOR */
	err = cbor_encode_shadow_object(buffer, buffer_size, &shadow, &encode_len);
	if (err) {
		LOG_ERR("cbor_encode_shadow_object, error: %d", err);
		return (err == ZCBOR_ERR_NO_PAYLOAD) ? -ENOMEM : -EFAULT;
	}

	*encoded_len = encode_len;

	LOG_DBG("Encoded config: update_interval=%u, sample_interval=%u, len=%zu",
		update_interval, sample_interval, encode_len);

	return 0;
}
