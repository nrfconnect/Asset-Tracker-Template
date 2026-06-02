/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _CLOUD_INTERNAL_H_
#define _CLOUD_INTERNAL_H_

#include <stdint.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Normalize a data sample timestamp to Unix time in milliseconds.
 *
 * Converts a timestamp expressed as uptime (in milliseconds) to Unix time when needed. A value
 * that is already Unix time is left untouched. When conversion is not possible the configured
 * CONFIG_APP_CLOUD_HANDLE_WRONG_SAMPLE_TIMESTAMPS_* policy is applied.
 *
 * This is shared by all data paths (battery, environmental, GNSS location) so they handle
 * timestamps identically and a timestamp that is already in Unix time is never double-converted.
 *
 * @param timestamp_ms In/out timestamp in milliseconds.
 * @return 0 on success or when the configured policy handled the value, negative errno otherwise.
 */
int cloud_timestamp_normalize(int64_t *timestamp_ms);

/**
 * @brief Private cloud channel message types.
 *
 * These messages are used for internal communication within the cloud module.
 */
enum priv_cloud_msg_type {
	CLOUD_CONNECTION_FAILED,
	CLOUD_CONNECTION_SUCCESS,
	CLOUD_NOT_AUTHENTICATED,
	CLOUD_PROVISIONING_FINISHED,
	CLOUD_PROVISIONING_FAILED,
	CLOUD_BACKOFF_EXPIRED,
	CLOUD_SEND_REQUEST_FAILED,
};

struct priv_cloud_msg {
	enum priv_cloud_msg_type type;
};

/* Private cloud channel - declared in cloud.c */
ZBUS_CHAN_DECLARE(priv_cloud_chan);

#ifdef __cplusplus
}
#endif

#endif /* _CLOUD_INTERNAL_H_ */
