/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _CLOUD_H_
#define _CLOUD_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Channels provided by this module */
ZBUS_CHAN_DECLARE(
	CLOUD_CHAN
);

struct cloud_payload {
	uint8_t buffer[CONFIG_APP_CLOUD_MQTT_PAYLOAD_BUFFER_MAX_SIZE];
	/* Length of the data stored inside the buffer */
	size_t buffer_data_len;
};

struct cloud_shadow_response {
	uint8_t buffer[CONFIG_APP_CLOUD_MQTT_SHADOW_RESPONSE_BUFFER_MAX_SIZE];
	/* Length of the data stored inside the buffer */
	size_t buffer_data_len;
};

enum cloud_msg_type {
	CLOUD_DISCONNECTED = 0x1,
	CLOUD_CONNECTED,
	CLOUD_PAYLOAD_JSON,

	/* Not implemented for MQTT */
	CLOUD_SHADOW_RESPONSE_DESIRED,
	CLOUD_SHADOW_RESPONSE_DELTA,
	CLOUD_SHADOW_RESPONSE_EMPTY_DESIRED,
	CLOUD_SHADOW_RESPONSE_EMPTY_DELTA,
	CLOUD_SHADOW_UPDATE_REPORTED,
	CLOUD_SHADOW_GET_DESIRED,
	CLOUD_SHADOW_GET_DELTA,
	CLOUD_PROVISIONING_REQUEST,
};

struct cloud_msg {
	enum cloud_msg_type type;
	union  {
		struct cloud_payload payload;
		struct cloud_shadow_response response;
	};
};

/* Cast a pointer to a message to a pointer to a cloud message */
#define MSG_TO_CLOUD_MSG_PTR(_msg)	((const struct cloud_msg *)_msg)

#ifdef __cplusplus
}
#endif

#endif /* _CLOUD_H_ */
