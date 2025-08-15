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
	 */
	STORAGE_MODE_BUFFER,

	/* Command to flush stored data one stored item at the time.
	 * The data will be pushed out as a STORAGE_DATA message on STORAGE_DATA_CHAN.
	 */
	STORAGE_FLUSH,

	/* Command to clear all stored data in the configured storage backend. */
	STORAGE_CLEAR,

	/* Command to request stored data using a pipe */
	STORAGE_PIPE_REQUEST,

	/* Command to print storage statistics.
	 * The command must be enabled with CONFIG_APP_STORAGE_SHELL_STATS.
	 */
	STORAGE_STATS,

	/* Output messages */

	/* Stored data being flushed as response to a STORAGE_FLUSH message */
	STORAGE_DATA,

	/* Response to a STORAGE_PIPE_REQUEST message. Indicates pipe is ready for reading.
	 * The `data_len` field indicates the total number of items available.
	 * The `session_id` field echoes back the session ID from the request.
	 * Use storage_pipe_read() with this session_id to consume data from the pipe.
	 */
	STORAGE_PIPE_AVAILABLE,

	/* No stored data available - pipe is empty. */
	STORAGE_PIPE_EMPTY,

	/* Error occurred during pipe operation. */
	STORAGE_PIPE_ERROR,

	/* Pipe is busy - cannot process request at this time. */
	STORAGE_PIPE_BUSY,

	/* Consumer finished with pipe session. */
	STORAGE_PIPE_CLOSE,

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

		/* Number of items available (for STORAGE_PIPE_AVAILABLE message) */
		size_t item_count;

		/* Session ID for pipe operations */
		uint32_t session_id;
	};

	/* For STORAGE_FLUSH, the length of the data in the buffer.
	 * For STORAGE_PIPE_AVAILABLE, the number of items available.
	 */
	size_t data_len;
};

/**
 * @brief Structure to define a storage data item
 *
 * This structure is used to hold data items read from the storage pipe.
 * No internal pointers or memory management required.
 */
struct storage_data_item {
	/* Type of data in the item.
	 * See DATA_SOURCE_LIST in storage_data_types.h for the list of data types.
	 */
	enum storage_data_type type;

	/* Inline data */
	union storage_data_type_buf data;
};

/**
 * @brief Read one item from storage pipe (convenience function)
 *
 * @details This is the ONLY direct API function provided by the storage module.
 * It reads stored data through the pipe interface, handling the header parsing
 * and data extraction automatically. All other operations (requesting pipe access,
 * closing sessions, etc.) must go through zbus messages.
 *
 * This function should only be called after receiving a STORAGE_PIPE_AVAILABLE
 * message in response to a STORAGE_PIPE_REQUEST. Use the same session_id that
 * was provided in the original request.
 *
 * @param session_id Session ID that was provided in the STORAGE_PIPE_REQUEST
 * @param out_item Pointer to structure where data will be copied
 * @param timeout Maximum time to wait for data
 *
 * @retval 0 on success, data copied to out_item
 * @retval -EAGAIN if no data available within timeout
 * @retval -ENODATA if pipe session ended (no more data)
 * @retval -EINVAL if out_item is NULL or invalid session_id
 * @retval -EIO if data corruption detected
 * @retval -EMSGSIZE if data too large for buffer
 */
int storage_pipe_read(uint32_t session_id, struct storage_data_item *out_item, k_timeout_t timeout);

/* Helper macro to convert message pointer */
#define MSG_TO_STORAGE_MSG(_msg)	(const struct storage_msg *)_msg

/* Declare the storage channels */
ZBUS_CHAN_DECLARE(STORAGE_CHAN);
ZBUS_CHAN_DECLARE(STORAGE_DATA_CHAN);

#ifdef __cplusplus
}
#endif

#endif /* _STORAGE_H_ */
