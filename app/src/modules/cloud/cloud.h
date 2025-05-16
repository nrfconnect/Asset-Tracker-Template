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
	uint8_t buffer[CONFIG_APP_CLOUD_PAYLOAD_BUFFER_MAX_SIZE];
	/* Length of the data stored inside the buffer */
	size_t buffer_data_len;
};

struct cloud_shadow_response {
	uint8_t buffer[CONFIG_APP_CLOUD_SHADOW_RESPONSE_BUFFER_MAX_SIZE];
	/* Length of the data stored inside the buffer */
	size_t buffer_data_len;
};

enum cloud_msg_type {
	CLOUD_DISCONNECTED = 0x1,
	CLOUD_CONNECTED,
	CLOUD_CONNECTION_ATTEMPT_COUNT_REACHED,
	CLOUD_PAYLOAD_JSON,
	CLOUD_POLL_SHADOW,

	/* Shadow response message. Contains the desired section of the shadow. */
	CLOUD_SHADOW_RESPONSE_DESIRED,

	/* Shadow response message. Contains the delta section of the shadow. */
	CLOUD_SHADOW_RESPONSE_DELTA,

	/* Triggers device provisioning. Sending this event puts the cloud module into
	 * provisioning mode, where it connects to the provisioning endpoint and checks
	 * for provisioning commands. Use this to onboard new devices to nRF Cloud with
	 * their attestation token, or to reprovision devices with new credentials when
	 * the old ones expire or when rotating credentials is required.
	 */
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
