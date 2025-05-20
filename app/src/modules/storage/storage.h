/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _STORAGE_H_
#define _STORAGE_H_

#include <stddef.h>
#include <zephyr/zbus/zbus.h>
#include "storage_data_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Message types for the storage channel */
enum storage_msg_type {
	/* Input message */
	/* Command to flush stored data */
	STORAGE_FLUSH = 0x1,

	/* Output messages */
	/* Stored data being flushed */
	STORAGE_DATA,
};

/* Message structure for the storage channel */
struct storage_msg {
	/* Type of message */
	enum storage_msg_type type;

	/* Type of data in buffer */
	enum storage_data_type data_type;

	/* Buffer for data */
	uint8_t buffer[CONFIG_APP_STORAGE_MSG_BUF_SIZE];

	/* Length of data in buffer */
	size_t buffer_data_len;
};

/* Helper macro to convert message pointer */
#define MSG_TO_STORAGE_MSG(_msg)	(*(const struct storage_msg *)_msg)

/* Declare the storage channel */
ZBUS_CHAN_DECLARE(STORAGE_CHAN);

#ifdef __cplusplus
}
#endif

#endif /* _STORAGE_H_ */
