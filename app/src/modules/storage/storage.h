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

	/* Request to enable pass through mode.
	 * Storage module will respond with STORAGE_MODE_PASSTHROUGH (confirmation) or
	 * STORAGE_MODE_CHANGE_REJECTED (if request cannot be fulfilled).
	 */
	STORAGE_MODE_PASSTHROUGH_REQUEST,

	/* Request to enable buffer mode.
	 * Storage module will respond with STORAGE_MODE_BUFFER (confirmation) or
	 * STORAGE_MODE_CHANGE_REJECTED (if request cannot be fulfilled).
	 */
	STORAGE_MODE_BUFFER_REQUEST,

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

	/* Command to print storage statistics.
	 * The command must be enabled with CONFIG_APP_STORAGE_SHELL_STATS.
	 */
	STORAGE_STATS,

	/* Output messages */

	/* Storage module is in pass-through mode.
	 * In this mode, the storage module will not store any data, but push it
	 * directly out as a STORAGE_DATA message on STORAGE_DATA_CHAN.
	 */
	STORAGE_MODE_PASSTHROUGH,

	/* Storage module is in buffer mode.
	 * In this mode, the storage module will store data in the configured storage backend.
	 * If the backend is not available, the data will be lost.
	 * If the storage is full, the oldest data will be removed to make space for new data.
	 */
	STORAGE_MODE_BUFFER,

	/* Stored data being flushed as response to a STORAGE_FLUSH message */
	STORAGE_DATA,

	/* Response to a STORAGE_BATCH_REQUEST message. Indicates batch is ready for reading.
	 * The `data_len` field contains the total number of items available.
	 * The `session_id` field echoes back the session ID from the request.
	 * Use storage_batch_read() with this session_id to consume data from the batch.
	 */
	STORAGE_BATCH_AVAILABLE,

	/* No stored data available - batch is empty. */
	STORAGE_BATCH_EMPTY,

	/* Error occurred during batch operation. */
	STORAGE_BATCH_ERROR,

	/* Batch is busy - cannot process request at this time. */
	STORAGE_BATCH_BUSY,

	/* Consumer finished with batch session. */
	STORAGE_BATCH_CLOSE,

	/* Mode change request was rejected due to safety constraints.
	 * For example, cannot switch to passthrough while batch session is active.
	 * Contains reject_reason field with details.
	 */
	STORAGE_MODE_CHANGE_REJECTED,
};

/**
 * @brief Reasons why a mode change request might be rejected
 */
enum storage_reject_reason {
	STORAGE_REJECT_UNKNOWN = 0,

	/* Cannot change to passthrough mode while batch session is active */
	STORAGE_REJECT_BATCH_ACTIVE,

	/* Cannot change mode due to internal error */
	STORAGE_REJECT_INTERNAL_ERROR,

	/* Request is invalid or malformed */
	STORAGE_REJECT_INVALID_REQUEST,
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

		/* Reason for mode change rejection (for STORAGE_MODE_CHANGE_REJECTED) */
		enum storage_reject_reason reject_reason;
	};

	/* Length/count field used by various message types (fixed-width for IPC stability):
	 * - STORAGE_BATCH_AVAILABLE: number of items available in batch
	 * - STORAGE_DATA: size of flushed data
	 */
	uint16_t data_len;
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
 * message in response to a STORAGE_BATCH_REQUEST. Use the same session_id that
 * was provided in the original request.
 *
 * session_id must be non-zero. Passing 0 returns -EINVAL.
 *
 * @param session_id Session ID that was provided in the STORAGE_BATCH_REQUEST
 * @param out_item Pointer to structure where data will be copied
 * @param timeout Maximum time to wait for data
 *
 * @retval 0 on success, data copied to out_item
 * @retval -EAGAIN if no data became available within timeout
 * @retval -EINVAL if out_item is NULL or invalid session_id
 * @retval -EIO if data corruption detected
 * @retval -EMSGSIZE if data too large for buffer
 */
int storage_batch_read(uint32_t session_id,
		       struct storage_data_item *out_item,
		       k_timeout_t timeout);

/* Helper macro to convert message pointer */
#define MSG_TO_STORAGE_MSG(_msg)	(const struct storage_msg *)_msg

/* Declare the storage channels */
ZBUS_CHAN_DECLARE(STORAGE_CHAN);
ZBUS_CHAN_DECLARE(STORAGE_DATA_CHAN);

#ifdef __cplusplus
}
#endif

#endif /* _STORAGE_H_ */
