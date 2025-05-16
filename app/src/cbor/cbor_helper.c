/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <errno.h>
#include <string.h>

#include "cbor_helper.h"
#include "device_shadow_decode.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(cbor_helper, CONFIG_APP_LOG_LEVEL);

int get_parameters_from_cbor_response(const uint8_t *cbor,
				      size_t len,
				      uint32_t *interval_sec,
				      uint32_t *command_type)
{
	int err;
	struct desired_object desired_object = { 0 };
	size_t decode_len = len;

	if (!cbor || !interval_sec || !command_type || len == 0) {
		return -EINVAL;
	}

	err = cbor_decode_desired_object(cbor, decode_len, &desired_object, &decode_len);
	if (err) {
		LOG_ERR("cbor_decode_desired_object, error: %d", err);
		LOG_HEXDUMP_ERR(cbor, len, "Unexpected CBOR data");
		return -EFAULT;
	}

	if (desired_object.config_present && desired_object.config.update_interval_present) {
		*interval_sec = desired_object.config.update_interval.update_interval;
	} else {
		LOG_DBG("Update interval parameter not present");
	}

	if (desired_object.command_present) {
		*command_type = desired_object.command.type;
		/* ID entry not used */
	} else {
		LOG_DBG("Command parameter not present");
	}

	return 0;
}
