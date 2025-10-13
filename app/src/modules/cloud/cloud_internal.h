/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _CLOUD_INTERNAL_H_
#define _CLOUD_INTERNAL_H_

#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Private cloud channel message types.
 *
 * These messages are used for internal communication within the cloud module.
 */
enum priv_cloud_msg {
	CLOUD_CONNECTION_FAILED,
	CLOUD_CONNECTION_SUCCESS,
	CLOUD_NOT_AUTHENTICATED,
	CLOUD_PROVISIONING_FINISHED,
	CLOUD_PROVISIONING_FAILED,
	CLOUD_BACKOFF_EXPIRED,
	CLOUD_SEND_REQUEST_FAILED,
};

/* Private cloud channel - declared in cloud.c */
ZBUS_CHAN_DECLARE(PRIV_CLOUD_CHAN);

#ifdef __cplusplus
}
#endif

#endif /* _CLOUD_INTERNAL_H_ */
