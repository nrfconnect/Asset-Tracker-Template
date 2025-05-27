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

	/* Command to purge all stored data */
	STORAGE_PURGE,

	/* Command to request stored data using a FIFO */
	STORAGE_FIFO_REQUEST,

	/* Purge the FIFO */
	STORAGE_FIFO_PURGE,

	/* Output messages */
	/* Stored data being flushed */
	STORAGE_DATA,

	/* FIFO for reading stored data */
	STORAGE_FIFO_AVAILABLE,

	/* FIFO is not available */
	STORAGE_FIFO_NOT_AVAILABLE,

	/* FIFO is empty, no stored data */
	STORAGE_FIFO_EMPTY,
};

/* Message structure for the storage channel */
struct storage_msg {
	/* Type of message */
	enum storage_msg_type type;

	/* Type of data in buffer */
	enum storage_data_type data_type;

	union {
		/* Buffer for data */
		uint8_t buffer[CONFIG_APP_STORAGE_MSG_BUF_SIZE];

		/* Pointer to the FIFO for reading data */
		struct k_fifo *fifo;
	};

	/* For STORAGE_FLUSH, the length of the data in the buffer.
	 * For STORAGE_FIFO_AVAILABLE, the number of elements in the FIFO.
	 */
	size_t data_len;
};
struct storage_data_chunk {
	/* The first word is reserved for internal use */
	void *fifo_reserved;

	/* Type of data in the chunk */
	enum storage_data_type type;

	/* Function that must be called when processing of the chunk is finished.
	 * Failure to call this function will result in a memory leak.
	 */
	void (*finished)(struct storage_data_chunk *chunk);

	/* Pointer to the data */
	union storage_data_type_ptr data;
};

/* Helper macro to convert message pointer */
#define MSG_TO_STORAGE_MSG(_msg)	(const struct storage_msg *)_msg

/* Declare the storage channel */
ZBUS_CHAN_DECLARE(STORAGE_CHAN);

#ifdef __cplusplus
}
#endif

#endif /* _STORAGE_H_ */
