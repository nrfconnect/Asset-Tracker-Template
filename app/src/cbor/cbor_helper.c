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

int get_update_parameters_from_cbor_response(const uint8_t *cbor,
					     size_t len,
					     uint32_t *interval_sec,
					     struct cloud_command *cmd)
{
	int err;
	struct config_object object = { 0 };
	size_t decode_len = len;

	if (!cbor || !interval_sec || !cmd || len == 0) {
		return -EINVAL;
	}

	err = cbor_decode_config_object(cbor, decode_len, &object, &decode_len);
	if (err) {
		LOG_ERR("cbor_decode_config_object, error: %d", err);
		LOG_HEXDUMP_ERR(cbor, len, "Unexpected CBOR data");
		return -EFAULT;
	}

	if (object.config_present && object.config.update_interval_present) {
		*interval_sec = object.config.update_interval.update_interval;
	}

	if (object.command_present) {
		if (object.command.name.len >= sizeof(cmd->name)) {
			LOG_ERR("Command name too long: %zu bytes", object.command.name.len);
			return -EINVAL;
		}

		memcpy(cmd->name, object.command.name.value, object.command.name.len);
		cmd->name[object.command.name.len] = '\0';
		cmd->id = object.command.id;

	}

	return 0;
}
