/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/net/coap.h>
#include <net/nrf_cloud_coap.h>
#include <nrf_cloud_coap_transport.h>

#include "cloud.h"
#include "cloud_configuration.h"
#include "cloud_internal.h"

LOG_MODULE_DECLARE(cloud, CONFIG_APP_CLOUD_LOG_LEVEL);

/* Number of bytes to check for empty buffer workaround */
#define SHADOW_BUFFER_EMPTY_CHECK_SIZE 10

ZBUS_CHAN_DECLARE(CLOUD_CHAN);

int cloud_configuration_poll(enum shadow_poll_type type)
{
	int err;
	bool delta = (type == SHADOW_POLL_DELTA);
	struct cloud_msg msg = {
		.type = delta ? CLOUD_SHADOW_RESPONSE_DELTA : CLOUD_SHADOW_RESPONSE_DESIRED,
		.response = {
			.buffer_data_len = sizeof(msg.response.buffer),
		},
	};

	LOG_DBG("Configuration: Requesting device shadow %s from cloud",
		delta ? "delta" : "desired");

	err = nrf_cloud_coap_shadow_get(msg.response.buffer,
					&msg.response.buffer_data_len,
					delta,
					COAP_CONTENT_FORMAT_APP_CBOR);
	if (err) {
		LOG_ERR("nrf_cloud_coap_shadow_get, error: %d", err);
		return err;
	}

	if (msg.response.buffer_data_len == 0) {
		LOG_DBG("Shadow %s section not present", delta ? "delta" : "desired");

		msg.type = delta ? CLOUD_SHADOW_RESPONSE_EMPTY_DELTA :
				   CLOUD_SHADOW_RESPONSE_EMPTY_DESIRED;

	} else if (!memcmp(msg.response.buffer, "\0\0\0\0\0\0\0\0\0\0",
			   SHADOW_BUFFER_EMPTY_CHECK_SIZE)) {
		/* Workaround: Sometimes nrf_cloud_coap_shadow_get() returns 0 even though
		 * obtaining the shadow failed. Ignore the payload if the first 10 bytes are zero.
		 */
		LOG_WRN("Returned buffer is empty, ignore");
		return -ENODATA;
	}

	err = zbus_chan_pub(&CLOUD_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		return err;
	}

	return 0;
}

int cloud_configuration_reported_update(const uint8_t *buffer, size_t buffer_len)
{
	int err;

	if (!buffer || buffer_len == 0) {
		return -EINVAL;
	}

	LOG_DBG("Configuration: Reporting config to cloud");

	err = nrf_cloud_coap_patch("state/reported", NULL,
				   buffer, buffer_len,
				   COAP_CONTENT_FORMAT_APP_CBOR,
				   true,
				   NULL,
				   NULL);
	if (err) {
		LOG_ERR("nrf_cloud_coap_patch (config report), error: %d", err);
		return err;
	}

	return 0;
}
