/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _CLOUD_MODULE_H_
#define _CLOUD_MODULE_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Channels provided by this module */
ZBUS_CHAN_DECLARE(
	CLOUD_CHAN,
	PAYLOAD_CHAN
);

struct cloud_payload {
	uint8_t buffer[CONFIG_APP_PAYLOAD_CHANNEL_BUFFER_MAX_SIZE];
	size_t buffer_len;
};

enum cloud_msg_type {
	CLOUD_DISCONNECTED = 0x1,
	CLOUD_CONNECTED_READY_TO_SEND,
	CLOUD_CONNECTED_PAUSED,
	CLOUD_CONNECTION_ATTEMPT_COUNT_REACHED,
	CLOUD_PAYLOAD_JSON,
	CLOUD_POLL_SHADOW,
};

struct cloud_msg {
	enum cloud_msg_type type;
	struct cloud_payload payload;
};

#define MSG_TO_CLOUD_MSG(_msg)	(*(const struct cloud_msg *)_msg)
#define MSG_TO_PAYLOAD(_msg)	((struct cloud_payload *)_msg)

#ifdef __cplusplus
}
#endif

#endif /* _CLOUD_MODULE_H_ */
