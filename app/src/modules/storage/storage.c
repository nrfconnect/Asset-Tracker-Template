/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/smf.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/sys/iterable_sections.h>
#include <string.h>
#include <stdint.h>

#include "storage.h"
#include "storage_backend.h"
#include "storage_data_types.h"
#include "app_common.h"

#ifdef CONFIG_APP_POWER
#include "power.h"
#endif
#ifdef CONFIG_APP_ENVIRONMENTAL
#include "environmental.h"
#endif
#ifdef CONFIG_APP_LOCATION
#include "location.h"
#endif

/* Register log module */
LOG_MODULE_REGISTER(storage, CONFIG_APP_STORAGE_LOG_LEVEL);

/* Timeout for pipe operations */
#define STORAGE_PIPE_TIMEOUT_MS	50

/* Register zbus subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(storage_subscriber);

/* Ensure that the watchdog timeout is greater than the message processing timeout
 * and the watchdog timeout is greater than the watchdog margin.
 */
BUILD_ASSERT(CONFIG_APP_STORAGE_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_STORAGE_MSG_PROCESSING_TIMEOUT_SECONDS);

/* Calculate the maximum message size from the list of channels */

/* Create channel list from DATA_SOURCE_LIST.
 * This is used to calculate the maximum message size and to add observers.
 * The DATA_SOURCE_LIST macro is defined in `storage_data_types.h`.
 */

/* Calculate maximum size needed for any message type */
#define STORAGE_MSG_SIZE_OF_TYPE(_name, _chan, _msg_type, ...)	sizeof(_msg_type),

#define STORAGE_MAX_MSG_SIZE_FROM_LIST(_DATA_SOURCE_LIST_LIST)	\
	MAX_N(_DATA_SOURCE_LIST_LIST(STORAGE_MSG_SIZE_OF_TYPE) 0)

/* Use the larger of: largest message type or storage_msg struct */
#define MAX_MSG_SIZE	MAX(STORAGE_MAX_MSG_SIZE_FROM_LIST(DATA_SOURCE_LIST), \
			    sizeof(struct storage_msg))

/**
 * @brief Add storage_subscriber as an observer to a channel
 *
 * This macro is used with DATA_SOURCE_LIST to add storage_subscriber as an observer
 * to each enabled channel. When DATA_SOURCE_LIST(ADD_OBSERVERS) is expanded, it:
 *
 * 1. Takes each enabled entry from DATA_SOURCE_LIST
 * 2. Passes the entry's parameters to this macro
 * 3. Creates a ZBUS_CHAN_ADD_OBS call using just the channel parameter
 *
 * For example, with CONFIG_APP_POWER enabled, the expansion looks like:
 *
 * Step 1: DATA_SOURCE_LIST expands to:
 *   ADD_OBSERVERS(battery, POWER_CHAN, struct power_msg, double, battery_check, battery_extract)
 *
 * Step 2: ADD_OBSERVERS expands to:
 *   ZBUS_CHAN_ADD_OBS(POWER_CHAN, storage_subscriber, 0)
 *
 * This process repeats for each enabled module in DATA_SOURCE_LIST.
 *
 * @param _n Name of the data type (unused in this macro)
 * @param _chan Channel to add observer to (used to create ZBUS_CHAN_ADD_OBS)
 * @param _t Message type (unused in this macro)
 * @param _dt Data type to store (unused in this macro)
 * @param _c Check function (unused in this macro)
 * @param _e Extract function (unused in this macro)
 */
#define ADD_OBSERVERS(_n, _chan, _t, _dt, _c, _e) ZBUS_CHAN_ADD_OBS(_chan, storage_subscriber, 0);

/* Add storage_subscriber as observer to each enabled channel.
 * The data source list is defined in `storage_data_types.h`.
 */
DATA_SOURCE_LIST(ADD_OBSERVERS);

/* Create the storage channel */
ZBUS_CHAN_DEFINE(STORAGE_CHAN,
		 struct storage_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Create the storage data channel */
ZBUS_CHAN_DEFINE(STORAGE_DATA_CHAN,
		 struct storage_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Forward declarations of state handlers */
static void state_running_entry(void *o);
static void state_running_run(void *o);
static void state_passthrough_run(void *o);
static void state_buffer_run(void *o);
static void state_buffer_idle_run(void *o);
static void state_buffer_pipe_active_entry(void *o);
static void state_buffer_pipe_active_run(void *o);
static void state_buffer_pipe_active_exit(void *o);

/* Storage pipe for streaming data to consumers */
K_PIPE_DEFINE(storage_pipe, CONFIG_APP_STORAGE_BATCH_BUFFER_SIZE, 4);

/* Storage pipe item header (compact, fixed-width) */
struct storage_pipe_header {
	uint8_t type;
	uint16_t data_size;
};

/* Pipe session tracking */
struct pipe_session {
	uint32_t session_id;
	size_t total_items;
	size_t items_sent;
	bool more_data;
};

/* Storage module state object */
struct storage_state {
	/* This must be first */
	struct smf_ctx ctx;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;

	/* Last received message */
	uint8_t msg_buf[MAX_MSG_SIZE];

	/* Current batch session state */
	struct pipe_session current_session;
};

/* Defining the storage module states */
enum storage_module_state {
	STATE_RUNNING,
	STATE_PASSTHROUGH,
	STATE_BUFFER,
	STATE_BUFFER_IDLE,
	STATE_BUFFER_PIPE_ACTIVE,
};

/* Construct state table */
static const struct smf_state states[] = {
	[STATE_RUNNING] =
#ifdef CONFIG_APP_STORAGE_INITIAL_MODE_PASSTHROUGH
		SMF_CREATE_STATE(state_running_entry, state_running_run, NULL,
				 NULL, /* No parent state */
				 &states[STATE_PASSTHROUGH]), /* Initial transition */
#elif IS_ENABLED(CONFIG_APP_STORAGE_INITIAL_MODE_BUFFER)
		SMF_CREATE_STATE(state_running_entry, state_running_run, NULL,
				 NULL, /* No parent state */
				 &states[STATE_BUFFER]), /* Initial transition */
#endif /* CONFIG_APP_STORAGE_INITIAL_MODE_BUFFER */
	[STATE_PASSTHROUGH] =
		SMF_CREATE_STATE(NULL, state_passthrough_run, NULL,
				 &states[STATE_RUNNING],
				 NULL),
	[STATE_BUFFER] =
		SMF_CREATE_STATE(NULL, state_buffer_run, NULL,
				 &states[STATE_RUNNING],
				 &states[STATE_BUFFER_IDLE]),
	[STATE_BUFFER_IDLE] =
		SMF_CREATE_STATE(NULL, state_buffer_idle_run, NULL,
				 &states[STATE_BUFFER],
				 NULL),
	[STATE_BUFFER_PIPE_ACTIVE] =
		SMF_CREATE_STATE(state_buffer_pipe_active_entry, state_buffer_pipe_active_run,
				 state_buffer_pipe_active_exit,
				 &states[STATE_BUFFER],
				 NULL),
};

/* Static helper functions */
static void task_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

/*
 * Write all bytes to pipe. The provided timeout is reused for each attempt to
 * keep logic simple and predictable. The helper is needed because
 * k_pipe_write() does not guarantee that all bytes will be written. Returns
 * number of bytes written if successful, negative error code if failed.
 */
static int pipe_write_all(struct k_pipe *pipe,
	const uint8_t *buf,
	size_t len,
	k_timeout_t timeout)
{
	int ret;
	size_t written_total = 0;

	while (written_total < len) {
		ret = k_pipe_write(pipe, buf + written_total, len - written_total, timeout);
		if (ret < 0) {
			return ret;
		}

		if (ret == 0) {
			return -EAGAIN;
		}

		written_total += (size_t)ret;
	}

	return (int)written_total;
}

/*
 * Read exact number of bytes from pipe. The provided timeout is reused for each
 * attempt to keep logic simple and predictable.
 * Returns number of bytes read if successful, negative error code if failed.
 */
static int pipe_read_exact(struct k_pipe *pipe,
	uint8_t *buf,
	size_t len,
	k_timeout_t timeout)
{
	int ret;
	size_t read_total = 0;

	while (read_total < len) {
		ret = k_pipe_read(pipe, buf + read_total, len - read_total, timeout);
		if (ret < 0) {
			return ret;
		}

		if (ret == 0) {
			return -EAGAIN;
		}

		read_total += (size_t)ret;
	}

	return (int)read_total;
}

static void handle_data_message(const struct storage_data *type,
				const uint8_t *buf)
{
	int err;
	uint8_t data[STORAGE_MAX_DATA_SIZE];
	const struct storage_backend *backend = storage_backend_get();

	LOG_DBG("Handle data message for %s", type->name);

	if (!type->should_store(buf)) {
		return;
	}

	type->extract_data(buf, (void *)data);

	err = backend->store(type, (const void *)data, type->data_size);
	if (err) {
		LOG_ERR("Failed to store %s data, error: %d", type->name, err);
	}
}

static void passthrough_data_msg(const struct storage_data *type,
				  const uint8_t *buf)
{
	int err;
	struct storage_msg msg = {
		.type = STORAGE_DATA,
		.data_type = type->data_type,
		.data_len = (uint16_t)MIN(type->data_size, (size_t)UINT16_MAX),
	};

	LOG_DBG("Passthrough data message for %s", type->name);

	/* Passthrough only relevant data */
	if (!type->should_store(buf)) {
		return;
	}

	type->extract_data(buf, (void *)msg.buffer);

	err = zbus_chan_pub(&STORAGE_DATA_CHAN, &msg, K_MSEC(STORAGE_PIPE_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to publish %s data, error: %d", type->name, err);
		SEND_FATAL_ERROR();
	}
}

static void flush_stored_data(void)
{
	int count;
	struct storage_msg msg = {0};
	const struct storage_backend *backend = storage_backend_get();
	uint8_t data[STORAGE_MAX_DATA_SIZE];

	STRUCT_SECTION_FOREACH(storage_data, type) {
		count = backend->count(type);
		if (count < 0) {
			LOG_ERR("Failed to get count for %p, error: %d", type->name, count);
			continue;
		}

		LOG_DBG("Flushing %d %s records", count, type->name);

		while (count > 0) {
			int ret;

			msg.type = STORAGE_DATA;
			msg.data_type = type->data_type;

			ret = backend->retrieve(type, data, sizeof(data));
			if (ret < 0) {
				LOG_ERR("Failed to retrieve %s data, error: %d", type->name, ret);
				break;
			}

			__ASSERT_NO_MSG(ret <= sizeof(data));
			__ASSERT_NO_MSG(ret == type->data_size);

			memcpy((void *)msg.buffer, data, ret);

			msg.data_len = (uint16_t)ret;

			ret = zbus_chan_pub(&STORAGE_DATA_CHAN, &msg,
					    K_MSEC(STORAGE_PIPE_TIMEOUT_MS));
			if (ret) {
				LOG_ERR("Failed to publish %s data, error: %d", type->name, ret);
				SEND_FATAL_ERROR();
			}

			count--;
		}
	}
}

static void storage_clear(void)
{
	int err;

	LOG_DBG("Purging storage");

	/* Clear all stored data */
	err = storage_backend_get()->clear();
	if (err) {
		LOG_ERR("Failed to clear storage backend, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

/* Drain any remaining data from the pipe */
static void drain_pipe(void)
{
	uint8_t dummy_buffer[64];

	while (k_pipe_read(&storage_pipe, dummy_buffer, sizeof(dummy_buffer), K_NO_WAIT) > 0) {
		/* Drain pipe */
	}
}

/* Send batch response message with session_id and optional data_len */
static void send_batch_response(enum storage_msg_type response_type,
			       uint32_t session_id,
			       size_t data_len,
			       bool more_data)
{
	int err;
	struct storage_msg response_msg = { 0 };

	response_msg.type = response_type;
	response_msg.session_id = session_id;
	response_msg.data_len = (uint16_t)MIN(data_len, (size_t)UINT16_MAX);
	response_msg.more_data = more_data;

	err = zbus_chan_pub(&STORAGE_CHAN, &response_msg, K_MSEC(STORAGE_PIPE_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to send batch response type %d: %d", response_type, err);
		SEND_FATAL_ERROR();
	}
}

/* Convenience wrappers for batch responses */
static void send_batch_busy_response(uint32_t session_id)
{
	send_batch_response(STORAGE_BATCH_BUSY, session_id, 0, false);
}

static void send_batch_empty_response(uint32_t session_id)
{
	send_batch_response(STORAGE_BATCH_EMPTY, session_id, 0, false);
}

static void send_batch_error_response(uint32_t session_id)
{
	send_batch_response(STORAGE_BATCH_ERROR, session_id, 0, false);
}

static void send_batch_available_response(uint32_t session_id, size_t item_count,
					  bool more_available)
{
	send_batch_response(STORAGE_BATCH_AVAILABLE, session_id, item_count, more_available);
}

/* Send mode confirmation */
static void send_mode_confirmed(enum storage_msg_type confirmed_type)
{
	int err;
	struct storage_msg confirm_msg = {
		.type = confirmed_type,
	};

	err = zbus_chan_pub(&STORAGE_CHAN, &confirm_msg, K_MSEC(STORAGE_PIPE_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to send mode confirmation: %d", err);
		SEND_FATAL_ERROR();
	}
}

/* Send mode rejection */
static void send_mode_rejected(enum storage_reject_reason reason)
{
	int err;
	struct storage_msg reject_msg = {
		.type = STORAGE_MODE_CHANGE_REJECTED,
		.reject_reason = reason,
	};

	err = zbus_chan_pub(&STORAGE_CHAN, &reject_msg, K_MSEC(STORAGE_PIPE_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to send mode rejection: %d", err);
		SEND_FATAL_ERROR();
	}
}

/* Populate the pipe with all stored data
 *
 * @return 0 on success (all data written or pipe full)
 * @return -EIO on data retrieval or size validation error
 */
static int populate_pipe(struct storage_state *state_object)
{
	const struct storage_backend *backend = storage_backend_get();
	size_t total_bytes_sent = 0;

	state_object->current_session.more_data = false;

	/* Populate pipe with all stored data */
	STRUCT_SECTION_FOREACH(storage_data, type) {
		int count = backend->count(type);

		while (count > 0) {
			int ret;
			size_t total_size;
			uint8_t item_buffer[sizeof(struct storage_pipe_header) +
					    STORAGE_MAX_DATA_SIZE];
			struct storage_pipe_header *header =
				(struct storage_pipe_header *)item_buffer;
			uint8_t *data = item_buffer + sizeof(struct storage_pipe_header);

			/* Peek at size without copying data (data = NULL) */
			ret = backend->peek(type, NULL, 0);
			if (ret == -EAGAIN) {
				/* No more data of this type */
				break;
			} else if (ret < 0) {
				LOG_ERR("Failed to peek %s data size: %d", type->name, ret);

				return -EIO;
			}

			/* Prepare header with actual size */
			header->type = (uint8_t)type->data_type;

			if ((ret < 0) || (ret > (int)STORAGE_MAX_DATA_SIZE) ||
			    (ret > UINT16_MAX)) {
				LOG_ERR("Invalid data size for header: %d", ret);

				return -EIO;
			}

			header->data_size = (uint16_t)ret;

			/* Calculate exact total size needed using actual data size */
			total_size = sizeof(struct storage_pipe_header) + ret;
			if (total_size > sizeof(item_buffer)) {
				LOG_ERR("Combined data too large: %zu > %zu",
					total_size, sizeof(item_buffer));

				return -EIO;
			}

			/* Check if exact size fits in remaining pipe buffer space */
			if (total_bytes_sent + total_size > CONFIG_APP_STORAGE_BATCH_BUFFER_SIZE) {
				/* Pipe buffer full - stop here without consuming data */
				LOG_DBG("Pipe buffer full");

				state_object->current_session.more_data = true;

				break;
			}

			/* Now that we know it fits, retrieve the data from backend */
			ret = backend->retrieve(type, data, STORAGE_MAX_DATA_SIZE);
			if (ret < 0) {
				LOG_ERR("Failed to retrieve %s data after peek: %d",
					type->name, ret);

				return -EIO;
			}

			/* Sanity check: retrieved size should match peeked size */
			__ASSERT_NO_MSG(ret == (int)header->data_size);

			/* Write combined buffer atomically to pipe */
			ret = pipe_write_all(&storage_pipe, item_buffer, total_size,
					     K_MSEC(STORAGE_PIPE_TIMEOUT_MS));
			if (ret < 0) {
				/* This should never happen since we checked space above */
				LOG_ERR("Unexpected pipe write failure after space check: %d", ret);

				return -EIO;
			}

			__ASSERT_NO_MSG(ret == (int)total_size);

			/* Update session progress and byte tracking */
			state_object->current_session.items_sent++;
			total_bytes_sent += total_size;

			count--;
		}
	}

	LOG_DBG("Batch population complete for session 0x%X: %zu/%zu items",
		state_object->current_session.session_id,
		state_object->current_session.items_sent,
		state_object->current_session.total_items);

	return 0;
}

/* Start a new batch session.
 * If the batch is empty, STORAGE_BATCH_EMPTY is sent and the session is not started.
 * If the batch is not empty, the session is started and STORAGE_BATCH_AVAILABLE is sent.
 * Returns 0 if successful, -ENODATA if batch is empty, and other error codes if failed.
 */
static int start_batch_session(struct storage_state *state_object,
			       const struct storage_msg *request_msg)
{
	int err;
	const struct storage_backend *backend = storage_backend_get();
	size_t total_items = 0;

	/* Enforce non-zero session id */
	if (request_msg->session_id == 0U) {
		send_batch_error_response(request_msg->session_id);
		return -EINVAL;
	}

	/* Count total items available */
	STRUCT_SECTION_FOREACH(storage_data, type) {
		int count = backend->count(type);

		if (count > 0) {
			total_items += count;
		}
	}

	if (total_items == 0) {
		send_batch_empty_response(request_msg->session_id);

		return -ENODATA;
	}

	/* Clear any stale data from pipe before starting new session */
	drain_pipe();

	/* Start new session using requester's session ID */
	state_object->current_session.session_id = request_msg->session_id;
	state_object->current_session.total_items = total_items;
	state_object->current_session.items_sent = 0;

	/* Try to populate the pipe */
	err = populate_pipe(state_object);
	if (err < 0) {
		/* Error occurred during pipe population */
		send_batch_error_response(request_msg->session_id);

		LOG_ERR("Failed to populate pipe for session 0x%X: %d",
			state_object->current_session.session_id, err);

		return err;
	}

	/* Success - pipe populated (fully or partially) */
	send_batch_available_response(request_msg->session_id,
				      state_object->current_session.items_sent,
				      state_object->current_session.more_data);

	LOG_DBG("Started batch session (session_id 0x%X), %zu items in batch (%zu total)",
		state_object->current_session.session_id,
		state_object->current_session.items_sent,
		total_items);

	return 0;
}

/* New API: Read one item from storage batch */
int storage_batch_read(struct storage_data_item *out_item, k_timeout_t timeout)
{
	struct storage_pipe_header header;
	int ret;

	if (!out_item) {
		return -EINVAL;
	}

	/* Session validation is implicit - if there's no active session,
	 * the pipe will be empty and this function will return -EAGAIN.
	 */

	/* First, try to read just the header to get the data size */
	ret = pipe_read_exact(&storage_pipe, (uint8_t *)&header,
			       sizeof(header), timeout);
	if (ret < 0) {
		return ret;  /* -EAGAIN (no data in timeout) or other error */
	}

	if (ret != sizeof(header)) {
		LOG_ERR("Incomplete header read: %d/%zu bytes",
			ret, sizeof(header));
		return -EIO;
	}

	/* Validate header */
	if ((size_t)header.data_size > sizeof(out_item->data)) {
		LOG_ERR("Data size too large: %u > %zu",
			header.data_size, sizeof(out_item->data));
		return -EMSGSIZE;
	}

	/* Read the data portion */
	ret = pipe_read_exact(&storage_pipe, (uint8_t *)&out_item->data,
			  (size_t)header.data_size, timeout);
	if (ret < 0) {
		LOG_ERR("Failed to read data from pipe: %d", ret);
		return ret;
	}

	if (ret != header.data_size) {
		LOG_ERR("Incomplete data read: %d/%u bytes",
			ret, header.data_size);
		return -EIO;
	}

	/* Fill output structure */
	out_item->type = header.type;

	LOG_DBG("Read storage item: type=%u, size=%u", header.type, header.data_size);

	/* Session will be closed via explicit STORAGE_BATCH_CLOSE messages */

	return 0;
}

#ifdef CONFIG_APP_STORAGE_SHELL_STATS
static void handle_storage_stats(void)
{
	const struct storage_backend *backend = storage_backend_get();
	int total_records = 0;
	int total_types = 0;

	LOG_INF("=== Storage Statistics ===");
	LOG_INF("Backend: %s", backend ? "Available" : "Not available");

	if (!backend) {
		LOG_ERR("No storage backend available");
		return;
	}

	/* Iterate through all registered storage data types */
	STRUCT_SECTION_FOREACH(storage_data, type) {
		int count = backend->count(type);

		if (count < 0) {
			LOG_ERR("Failed to get count for %s, error: %d", type->name, count);
			continue;
		}

		LOG_INF("%s: %d records", type->name, count);

		total_records += count;
		total_types++;
	}

	LOG_INF("Total: %d records across %d data types", total_records, total_types);
	LOG_INF("Max records per type: %d", CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE);
	LOG_INF("========================");
}
#endif /* CONFIG_APP_STORAGE_SHELL_STATS */

/* Handler for STATE_RUNNING */
static void state_running_entry(void *o)
{
	int err;
	const struct storage_backend *backend = storage_backend_get();

	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	err = backend->init();
	if (err) {
		LOG_ERR("Failed to initialize storage backend, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

static void state_running_run(void *o)
{
	const struct storage_state *state_object = (const struct storage_state *)o;
	const struct storage_msg *msg = (const struct storage_msg *)state_object->msg_buf;

	LOG_DBG("%s", __func__);

	if (state_object->chan == &STORAGE_CHAN) {
		switch (msg->type) {
		case STORAGE_CLEAR:
			/* Clear all stored data */
			storage_clear();
			break;

		case STORAGE_FLUSH:
			flush_stored_data();
			break;
#ifdef CONFIG_APP_STORAGE_SHELL_STATS
		case STORAGE_STATS:
			/* Show storage statistics */
			handle_storage_stats();
			break;
#endif /* CONFIG_APP_STORAGE_SHELL_STATS */
		default:
			break;
		}
	}
}

static void state_passthrough_run(void *o)
{
	const struct storage_state *state_object = (const struct storage_state *)o;
	const struct storage_msg *msg = (const struct storage_msg *)state_object->msg_buf;

	/* Check if message is from a registered data type */
	STRUCT_SECTION_FOREACH(storage_data, type) {
		if (state_object->chan == type->chan) {
			passthrough_data_msg(type, state_object->msg_buf);

			return;
		}
	}

	if (state_object->chan == &STORAGE_CHAN) {
		switch (msg->type) {
		case STORAGE_MODE_PASSTHROUGH_REQUEST:
			LOG_DBG("Already in passthrough mode, sending confirmation");
			send_mode_confirmed(STORAGE_MODE_PASSTHROUGH);
			smf_set_handled(SMF_CTX(state_object));

			break;
		case STORAGE_MODE_BUFFER_REQUEST:
			LOG_DBG("Switching to buffer mode (with confirmation)");
			send_mode_confirmed(STORAGE_MODE_BUFFER);
			smf_set_state(SMF_CTX(state_object), &states[STATE_BUFFER_IDLE]);

			break;
		case STORAGE_BATCH_REQUEST:
			send_batch_error_response(msg->session_id);

			break;
		default:
			break;
		}
	}
}

static void state_buffer_run(void *o)
{
	struct storage_state *state_object = (struct storage_state *)o;
	const struct storage_msg *msg = (const struct storage_msg *)state_object->msg_buf;

	/* Handle common buffer state messages */
	if (state_object->chan == &STORAGE_CHAN) {
		if (msg->type == STORAGE_MODE_BUFFER_REQUEST) {
			LOG_DBG("Already in buffer mode, sending confirmation");
			send_mode_confirmed(STORAGE_MODE_BUFFER);
			smf_set_handled(SMF_CTX(state_object));

			return;
		}
	}
}

static void state_buffer_idle_run(void *o)
{
	struct storage_state *state_object = (struct storage_state *)o;
	const struct storage_msg *msg = (const struct storage_msg *)state_object->msg_buf;

	LOG_DBG("%s", __func__);

	/* Check if message is from a registered data type */
	STRUCT_SECTION_FOREACH(storage_data, type) {
		if (state_object->chan == type->chan) {
			handle_data_message(type, state_object->msg_buf);

			return;
		}
	}

	if (state_object->chan == &STORAGE_CHAN) {
		switch (msg->type) {
		case STORAGE_MODE_PASSTHROUGH_REQUEST:
			LOG_DBG("Switching to passthrough mode (with confirmation)");
			send_mode_confirmed(STORAGE_MODE_PASSTHROUGH);
			smf_set_state(SMF_CTX(state_object), &states[STATE_PASSTHROUGH]);

			return;
		case STORAGE_BATCH_REQUEST:
			LOG_DBG("Batch request received, switching to batch active state");
			/* Set up session ID for the upcoming batch session */
			state_object->current_session.session_id = msg->session_id;
			smf_set_state(SMF_CTX(state_object), &states[STATE_BUFFER_PIPE_ACTIVE]);

			return;
		default:
			break;
		}
	}
}

static void state_buffer_pipe_active_entry(void *o)
{
	int err;
	struct storage_state *state_object = (struct storage_state *)o;
	const struct storage_msg *msg = (const struct storage_msg *)state_object->msg_buf;

	LOG_DBG("%s", __func__);

	err = start_batch_session(state_object, msg);
	if (err == -ENODATA) {
		/* No data available, report it */
		send_batch_empty_response(msg->session_id);

		return;
	} else if (err) {
		LOG_ERR("Failed to start pipe session: %d", err);
		send_batch_error_response(msg->session_id);

		return;
	}

	LOG_DBG("Batch session started, session_id: %u", state_object->current_session.session_id);
}

static void state_buffer_pipe_active_run(void *o)
{
	struct storage_state *state_object = (struct storage_state *)o;
	const struct storage_msg *msg = (const struct storage_msg *)state_object->msg_buf;

	LOG_DBG("%s", __func__);

	if (state_object->chan == &STORAGE_CHAN) {
		switch (msg->type) {
		case STORAGE_CLEAR:
			LOG_WRN("Cannot clear storage while batch session is active");
			smf_set_handled(SMF_CTX(state_object));

			return;

		case STORAGE_BATCH_CLOSE:
			if (state_object->current_session.session_id == msg->session_id) {
				smf_set_state(SMF_CTX(state_object), &states[STATE_BUFFER_IDLE]);
			} else {
				LOG_WRN("Invalid session ID: 0x%X (current: 0x%X)",
					msg->session_id, state_object->current_session.session_id);
			}

			return;

		case STORAGE_BATCH_REQUEST:
			LOG_DBG("Batch request received, session_id: 0x%X", msg->session_id);

			if (state_object->current_session.session_id &&
			    (state_object->current_session.session_id != msg->session_id)) {
				send_batch_busy_response(msg->session_id);
				LOG_DBG("Session ID mismatch: 0x%X (current: 0x%X)",
					msg->session_id, state_object->current_session.session_id);

				return;
			}

			/* We allow multiple requests in the same session.
			 * The batch will be refreshed with new data.
			 */
			start_batch_session(state_object, msg);
			LOG_DBG("Session started: 0x%X", state_object->current_session.session_id);

			break;

		case STORAGE_MODE_PASSTHROUGH_REQUEST:
			LOG_WRN("Cannot change to passthrough mode while batch session is active");
			send_mode_rejected(STORAGE_REJECT_BATCH_ACTIVE);
			smf_set_handled(SMF_CTX(state_object));

			break;

		default:
			LOG_DBG("Ignoring message type: %d", msg->type);

			break;
		}
	}
}

static void state_buffer_pipe_active_exit(void *o)
{
	struct storage_state *state_object = (struct storage_state *)o;

	LOG_DBG("%s", __func__);

	/* Drain any remaining data from pipe */
	drain_pipe();

	/* Clear session state */
	memset(&state_object->current_session, 0, sizeof(state_object->current_session));
}

static void storage_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = CONFIG_APP_STORAGE_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC;
	const uint32_t execution_time_ms =
		(CONFIG_APP_STORAGE_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	struct storage_state storage_state = {0};

	LOG_DBG("Storage module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, task_wdt_callback,
				  (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		SEND_FATAL_ERROR();

		return;
	}

	err = zbus_chan_add_obs(&STORAGE_CHAN, &storage_subscriber,
				K_MSEC(STORAGE_PIPE_TIMEOUT_MS));
	if (err) {
		LOG_ERR("Failed to add observer to STORAGE_CHAN, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	/* Initialize the state machine */
	smf_set_initial(SMF_CTX(&storage_state), &states[STATE_RUNNING]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}

		err = zbus_sub_wait_msg(&storage_subscriber, &storage_state.chan,
				       storage_state.msg_buf, zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}

		err = smf_run_state(SMF_CTX(&storage_state));
		if (err) {
			LOG_ERR("smf_run_state(), error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}
	}
}

K_THREAD_DEFINE(storage_thread_id,
		CONFIG_APP_STORAGE_THREAD_STACK_SIZE,
		storage_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
