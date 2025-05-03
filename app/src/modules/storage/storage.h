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
	/* Input messages */

	/* Command to pass through data without buffering.
	 * When this mode is enabled, the storage module will not store any data, out push it
	 * directly out as a STORAGE_DATA message.
	 */
	STORAGE_MODE_PASSTHROUGH,


	/* In buffer mode, the storage module will store data in the configured storage backend.
	 * If the backend is not available, the data will be lost.
	 * If the storage is full, the oldest data will be removed to make space for new data.
	 * The data will be flushed to the FIFO when STORAGE_FLUSH_TO_FIFO is called.
	 */
	STORAGE_MODE_BUFFER,

	/* Command to flush stored data one stored item at the time. */
	STORAGE_FLUSH,

	/* Flush all stored data into a FIFO. The number of items flushed
	 * is limited by the FIFO size.
	 */
	STORAGE_FLUSH_TO_FIFO,

	/* Command to clear all stored data */
	STORAGE_CLEAR,

	/* Command to request stored data using a FIFO */
	STORAGE_FIFO_REQUEST,

	/* Clear all the data in the FIFO */
	STORAGE_FIFO_CLEAR,

	/* Command to get storage statistics.
	 * The command must be enabled with CONFIG_APP_STORAGE_SHELL_STATS.
	 */
	STORAGE_STATS,

	/* Output messages */
	/* Stored data being flushed as response to a STORAGE_FLUSH message */
	STORAGE_DATA,

	/* FIFO for reading stored data */
	STORAGE_FIFO_AVAILABLE,

	/* FIFO is not available.
	 * This can happen if there was an error when reading stored data into the FIFO.
	 */
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
	union storage_data_type_buf data;
};

/* Helper macro to convert message pointer */
#define MSG_TO_STORAGE_MSG(_msg)	(const struct storage_msg *)_msg

/* Declare the storage channel */
ZBUS_CHAN_DECLARE(STORAGE_CHAN);

#ifdef __cplusplus
}
#endif

#endif /* _STORAGE_H_ */
