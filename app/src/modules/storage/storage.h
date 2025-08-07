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

/* Message types for the storage channels */
enum storage_msg_type {
	/* Input messages */

	/* Command to enable pass through mode.
	 * When this mode is enabled, the storage module will not store any data,
	 * but push it directly out as a STORAGE_DATA message on STORAGE_DATA_CHAN.
	 */
	STORAGE_MODE_PASSTHROUGH,

	/* Command to enable buffer mode.
	 * In buffer mode, the storage module will store data in the configured storage backend.
	 * If the backend is not available, the data will be lost.
	 * If the storage is full, the oldest data will be removed to make space for new data.
	 * The data will be flushed to the FIFO when STORAGE_FIFO_REQUEST is called.
	 */
	STORAGE_MODE_BUFFER,

	/* Command to flush stored data one stored item at the time.
	 * The data will be pushed out as a STORAGE_DATA message on STORAGE_DATA_CHAN.
	 */
	STORAGE_FLUSH,

	/* Command to clear all stored data in the configured storage backend and the FIFO. */
	STORAGE_CLEAR,

	/* Command to request stored data using a FIFO */
	STORAGE_FIFO_REQUEST,

	/* Clear all the data in the FIFO. Data is not removed from the storage backend. */
	STORAGE_FIFO_CLEAR,

	/* Command to print storage statistics.
	 * The command must be enabled with CONFIG_APP_STORAGE_SHELL_STATS.
	 */
	STORAGE_STATS,

	/* Output messages */

	/* Stored data being flushed as response to a STORAGE_FLUSH message */
	STORAGE_DATA,

	/* Response to a STORAGE_FIFO_REQUEST message. This message contains a pointer to the FIFO
	 * in the `fifo` field. The FIFO is populated with stored data, which is removed from the
	 * storage backend upon transfer to the FIFO.
	 *
	 * The `data_len` field indicates the number of elements in the FIFO. The FIFO can hold
	 * a maximum of CONFIG_APP_STORAGE_FIFO_ITEM_COUNT elements. When the FIFO is at capacity,
	 * `data_len` will equal CONFIG_APP_STORAGE_FIFO_ITEM_COUNT, and the consumer should issue
	 * another STORAGE_FIFO_REQUEST to retrieve the next batch of data.
	 *
	 * IMPORTANT: The consumer must call the `finished` function for every element in the FIFO.
	 * Failure to do so will result in memory leaks.
	 */
	STORAGE_FIFO_AVAILABLE,

	/* FIFO is not available.
	 * This can happen if there was an error when reading stored data into the FIFO.
	 * The consumer should call STORAGE_FIFO_REQUEST again to try again.
	 */
	STORAGE_FIFO_NOT_AVAILABLE,

	/* FIFO is empty, no stored data to read. */
	STORAGE_FIFO_EMPTY,
};

/**
 * @brief Message structure for the storage channel
 *
 * This structure is used to define a message that is sent over the storage channels.
 */
struct storage_msg {
	/* Type of message */
	enum storage_msg_type type;

	/* Type of data in buffer */
	enum storage_data_type data_type;

	union {
		/* Buffer for incoming data over the channels that the storage module is
		 * subscribed to.
		 */
		uint8_t buffer[STORAGE_MAX_DATA_SIZE];

		/* Pointer to the FIFO for reading data in a STORAGE_FIFO_AVAILABLE message. */
		struct k_fifo *fifo;
	};

	/* For STORAGE_FLUSH, the length of the data in the buffer.
	 * For STORAGE_FIFO_AVAILABLE, the number of elements in the FIFO.
	 */
	size_t data_len;
};

/**
 * @brief Structure to define a storage FIFO item
 *
 * This structure is used to define the data elements that are added to the FIFO in a
 * STORAGE_FIFO_AVAILABLE message.
 */
struct storage_fifo_item {
	/* The first word is reserved for internal use */
	void *fifo_reserved;

	/* Type of data in the item.
	 * See DATA_SOURCE_LIST in storage_data_types.h for the list of data types.
	 */
	enum storage_data_type type;

	/* Function that must be called when processing of the item is finished.
	 * Failure to call this function will result in a memory leak.
	 */
	void (*finished)(struct storage_fifo_item *item);

	/* Slab the item was allocated from (used for freeing without lookups) */
	struct k_mem_slab *slab;

	/* Inline data */
	union storage_data_type_buf data;
};

/* Helper macro to convert message pointer */
#define MSG_TO_STORAGE_MSG(_msg)	(const struct storage_msg *)_msg

/* Declare the storage channels */
ZBUS_CHAN_DECLARE(STORAGE_CHAN);
ZBUS_CHAN_DECLARE(STORAGE_DATA_CHAN);

#ifdef __cplusplus
}
#endif

#endif /* _STORAGE_H_ */
