/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _STORAGE_H_
#define _STORAGE_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/zbus/zbus.h>
#include "storage_data_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Message types for the storage channels */
enum storage_msg_type {
	/* Input messages */

	/* Request to set buffer trigger limit.
	 * Sets the number of items in storage that will trigger a STORAGE_THRESHOLD_REACHED
	 * message.
	 * The message must contain the new trigger limit in the `data_len` field.
	 */
	STORAGE_SET_THRESHOLD,

	/* Command to flush stored data one stored item at the time.
	 * The data will be pushed out as a STORAGE_DATA message on STORAGE_DATA_CHAN.
	 */
	STORAGE_FLUSH,

	/* Command to clear all stored data in the configured storage backend. */
	STORAGE_CLEAR,

	/* Command to request stored data using batch access
	 * The request must contain a session_id that will be used to identify the batch session.
	 * The session_id must be the same for all messages in the same batch session.
	 * It is possible to request the batch multiple times in the same session.
	 * The batch will be refreshed with new data each time.
	 * At the end of the session, the batch must be closed with STORAGE_BATCH_CLOSE.
	 */
	STORAGE_BATCH_REQUEST,

	/* Consumer finished with batch session. */
	STORAGE_BATCH_CLOSE,

	/* Command to print storage statistics.
	 * The command must be enabled with CONFIG_APP_STORAGE_SHELL_STATS.
	 */
	STORAGE_STATS,

	/* Output messages */

	/* Number of items in storage >= trigger limit.
	 * The `data_len` field contains the total number of items in storage.
	 * This message is sent when the trigger limit is reached or exceeded.
	 */
	STORAGE_THRESHOLD_REACHED,

	/* Stored data being flushed as response to a STORAGE_FLUSH message */
	STORAGE_DATA,

	/* Response to a STORAGE_BATCH_REQUEST message. Indicates batch is ready for reading.
	 * The `data_len` field contains the total number of items available.
	 * The `session_id` field echoes back the session ID from the request.
	 * Use storage_batch_read() to consume data from the batch.
	 */
	STORAGE_BATCH_AVAILABLE,

	/* No stored data available - batch is empty.
	 * The batch session must still be closed with STORAGE_BATCH_CLOSE.
	 */
	STORAGE_BATCH_EMPTY,

	/* Error occurred during batch operation.
	 * The batch session must still be closed with STORAGE_BATCH_CLOSE.
	 */
	STORAGE_BATCH_ERROR,

	/* Batch is busy - cannot process request at this time. */
	STORAGE_BATCH_BUSY,
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

		/* Session ID for batch operations */
		uint32_t session_id;
	};

	/* Length/count field used by various message types:
	 * - STORAGE_BATCH_AVAILABLE: number of items available in batch
	 * - STORAGE_DATA: size of flushed data
	 */
	uint32_t data_len: 31;

	/* Indicates if there is more data to be read from the batch.
	 * Valid only for STORAGE_BATCH_AVAILABLE messages.
	 * This is used by the batch reader to determine if it should continue reading after
	 * the current batch has been read.
	 */
	bool more_data: 1;
};

/**
 * @brief Structure to define a storage data item
 *
 * This structure is used to hold data items read from storage batch access.
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
 * @brief Read one item from storage batch (convenience function)
 *
 * @details This is the ONLY direct API function provided by the storage module.
 * It reads stored data through the batch interface, handling the header parsing
 * and data extraction automatically. All other operations (requesting batch access,
 * closing sessions, etc.) must go through zbus messages.
 *
 * This function should only be called after receiving a STORAGE_BATCH_AVAILABLE
 * message in response to a STORAGE_BATCH_REQUEST.
 *
 * @param out_item Pointer to structure where data will be copied
 * @param timeout Maximum time to wait for data
 *
 * @retval 0 on success, data copied to out_item
 * @retval -EAGAIN if no data became available within timeout
 * @retval -EINVAL if out_item is NULL
 * @retval -EIO if data corruption detected
 * @retval -EMSGSIZE if data too large for buffer
 */
int storage_batch_read(struct storage_data_item *out_item,
		       k_timeout_t timeout);

/* Helper macro to convert message pointer */
#define MSG_TO_STORAGE_MSG_PTR(_msg)	(const struct storage_msg *)_msg

/* Declare the storage channels */
ZBUS_CHAN_DECLARE(STORAGE_CHAN);
ZBUS_CHAN_DECLARE(STORAGE_DATA_CHAN);

#ifdef __cplusplus
}
#endif

#endif /* _STORAGE_H_ */
