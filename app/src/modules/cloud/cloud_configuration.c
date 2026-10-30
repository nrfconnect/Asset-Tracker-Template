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

#include "app_common.h"
#include "cloud.h"
#include "cloud_configuration.h"
#include "cloud_internal.h"

LOG_MODULE_DECLARE(cloud, CONFIG_APP_CLOUD_LOG_LEVEL);

ZBUS_CHAN_DECLARE(cloud_chan);

#if defined(CONFIG_APP_CLOUD_SHADOW_CONTROL)
static void shadow_control_process(bool delta)
{
	static char buf[CONFIG_APP_CLOUD_SHADOW_CONTROL_BUFFER_MAX_SIZE];
	size_t buf_len = sizeof(buf);
	struct nrf_cloud_obj delta_obj = {0};
	int err;

	memset(buf, 0, sizeof(buf));

	err = nrf_cloud_coap_shadow_get(buf, &buf_len, delta, COAP_CONTENT_FORMAT_APP_JSON);
	if (err) {
		LOG_ERR("Error getting nrf cloud shadow: %d", err);
		return;
	}
	if (buf_len == 0 || buf[0] == '\0') {
		/* No data to process */
		return;
	}

	struct nrf_cloud_data in_data = {.ptr = buf, .len = buf_len};

	/* Control section updates are processed here, notably the memfaultModemKey. This function
	 * also will free delta_obj if there is no data left to process.
	 */
	err = nrf_cloud_coap_shadow_delta_process(&in_data, &delta_obj);
	if (err < 0) {
		LOG_ERR("Error processing shadow delta: %d", err);
		return;
	}
	if (err > 0) {
		/* There is still application data to be processed. Free the object here
		 * though since it will be re-requested and processed as CBOR later.
		 */
		nrf_cloud_obj_free(&delta_obj);
		return;
	}
}
#endif /* CONFIG_APP_CLOUD_SHADOW_CONTROL */

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

#if defined(CONFIG_APP_CLOUD_SHADOW_CONTROL)
	/* Apply and acknowledge the control section separately from the application configuration
	 * since the nrf cloud shadow control section requires processing it as JSON.
	 */
	shadow_control_process(delta);
#endif

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

	} else if (!memcmp(msg.response.buffer, "\0\0\0\0\0\0\0\0\0\0", 10)) {
		/* Workaround: Sometimes nrf_cloud_coap_shadow_get() returns 0 even though
		 * obtaining the shadow failed. Ignore the payload if the first 10 bytes are zero.
		 */
		LOG_WRN("Returned buffer is empty, ignore");
		return -ENODATA;
	}

	err = zbus_chan_pub(&cloud_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		return err;
	}

	return 0;
}

/*
 * cloud_configuration_reported_set() is used to clear the existing reported/config section in the
 * shadow and replace it with the new configuration. This is used when reporting the full device
 * configuration to the shadow.
 */
int cloud_configuration_reported_set(const uint8_t *buffer, size_t buffer_len)
{
	int err;

	if (!buffer || buffer_len == 0) {
		return -EINVAL;
	}

	/* Hardcoded {"config": null} to clear the reported section on the cloud shadow. This is
	 * used to clear out old configuration values from the shadow that are no longer used by the
	 * application.
	 */
	static const uint8_t clear_reported_payload[9] = {
		0xA1,                               /* map of 1 pair */
		0x66,                               /* text string of length 6 */
		0x63, 0x6F, 0x6E, 0x66, 0x69, 0x67, /* "config" */
		0xF6                                /* null */
	};

	err = nrf_cloud_coap_patch("state/reported", NULL, clear_reported_payload,
				   sizeof(clear_reported_payload), COAP_CONTENT_FORMAT_APP_CBOR,
				   true, NULL, NULL);
	if (err) {
		LOG_ERR("nrf_cloud_coap_patch (clear reported), error: %d", err);
		return err;
	}

	/* Update the reported section with the new configuration */
	err = nrf_cloud_coap_patch("state/reported", NULL, buffer, buffer_len,
				   COAP_CONTENT_FORMAT_APP_CBOR, true, NULL, NULL);
	if (err) {
		LOG_ERR("nrf_cloud_coap_patch (config report), error: %d", err);
		return err;
	}

	return 0;
}

/*
 * cloud_configuration_reported_update() is used to update the existing reported/config section in
 * the shadow with the new configuration. This is used when reporting only the delta changes to the
 * shadow.
 */
int cloud_configuration_reported_update(const uint8_t *buffer, size_t buffer_len)
{
	int err;

	if (!buffer || buffer_len == 0) {
		return -EINVAL;
	}

	LOG_DBG("Configuration: Reporting delta config to cloud");

	err = nrf_cloud_coap_patch("state/reported", NULL, buffer, buffer_len,
				   COAP_CONTENT_FORMAT_APP_CBOR, true, NULL, NULL);
	if (err) {
		LOG_ERR("nrf_cloud_coap_patch (delta config report), error: %d", err);
		return err;
	}

	return 0;
}
