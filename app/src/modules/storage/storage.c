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

#include "storage.h"
#include "storage_backend.h"
#include "storage_data_types.h"
#include "app_common.h"

#include "power.h"
#include "environmental.h"
#include "location.h"

/* Register log module */
LOG_MODULE_REGISTER(storage, CONFIG_APP_STORAGE_LOG_LEVEL);

/* Register zbus subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(storage_subscriber);

/* Calculate the maximum message size from the list of channels */
#define MAX_MSG_SIZE			MAX_MSG_SIZE_FROM_LIST(DATA_SOURCE_LIST)

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

/* Forward declarations of state handlers */
static void state_running_entry(void *o);
static void state_running_run(void *o);

K_FIFO_DEFINE(storage_fifo);

/* Defining the storage module states */
enum storage_module_state {
	STATE_RUNNING,
};

/* Construct state table */
static const struct smf_state states[] = {
	[STATE_RUNNING] = SMF_CREATE_STATE(state_running_entry, state_running_run, NULL,
					   NULL, /* No parent state */
					   NULL), /* No initial transition */
};

/* Storage module state object */
struct storage_state {
	/* This must be first */
	struct smf_ctx ctx;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;

	/* Last received message */
	uint8_t msg_buf[CONFIG_APP_STORAGE_MSG_BUF_SIZE];
};

/* Static helper function */
static void task_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

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

static void handle_data_message(const struct storage_data *type,
				const uint8_t *buf)
{
	int err;
	uint8_t data[CONFIG_APP_STORAGE_RECORD_SIZE];
	const struct storage_backend *backend = storage_backend_get();

	LOG_DBG("Handle data message for %s", type->name);

	if (!type->should_store(buf)) {
		return;
	}

	type->extract_data(buf, data);

	err = backend->store(type, data, type->data_size);
	if (err) {
		LOG_ERR("Failed to store %s data, error: %d", type->name, err);
	}
}

static void flush_stored_data(void)
{
	int err;
	int count;
	struct storage_msg msg = {0};
	const struct storage_backend *backend = storage_backend_get();
	uint8_t data[CONFIG_APP_STORAGE_RECORD_SIZE];

	/* De-register storage_subscriber from observing STORAGE_CHAN to avoid
	 * receiving messages while flushing. That would
	 */
	err = zbus_chan_rm_obs(&STORAGE_CHAN, &storage_subscriber, K_SECONDS(1));
	if (err) {
		LOG_ERR("Failed to remove observer from STORAGE_CHAN, error: %d", err);
	}

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

			memcpy(msg.buffer, data, ret);

			msg.data_len = ret;

			ret = zbus_chan_pub(&STORAGE_CHAN, &msg, K_SECONDS(1));
			if (ret) {
				LOG_ERR("Failed to publish %s data, error: %d", type->name, ret);

				break;
			}

			count--;
		}
	}

	/* Re-register storage_subscriber to observe STORAGE_CHAN */
	err = zbus_chan_add_obs(&STORAGE_CHAN, &storage_subscriber, K_SECONDS(1));
	if (err) {
		LOG_ERR("Failed to add observer to STORAGE_CHAN, error: %d", err);
	}
}

static void free_fifo_chunk(struct storage_data_chunk *chunk)
{
	__ASSERT_NO_MSG(chunk != NULL);

	if (!chunk) {
		LOG_ERR("Received NULL chunk to free");
		return;
	}

	STRUCT_SECTION_FOREACH(storage_data, type) {
		if (chunk->type == type->data_type) {
			k_mem_slab_free(type->slab, (void *)chunk);

			LOG_DBG("Freed FIFO chunk %p of type %d", (void *)chunk, chunk->type);

			return;
		}
	}

	LOG_ERR("Unknown data type in FIFO chunk %p: %d", (void *)chunk, chunk->type);
}

static int populate_fifo(void)
{
	const struct storage_backend *backend = storage_backend_get();
	int element_count_in_fifo = 0;

	/* Iterate over all the registered data types.
	 * For each type, retrieve stored data from the backend and store it in a mem slab.
	 * Reference the slab in a FIFO chunk (which is also a mem slab) and put it in the FIFO.
	 */
	STRUCT_SECTION_FOREACH(storage_data, type) {
		int ret;
		struct storage_data_chunk *chunk;
		int elements_left = backend->count(type);

		LOG_DBG("Populating FIFO for %s, elements left: %d", type->name, elements_left);

		if (elements_left == 0) {
			LOG_DBG("No data to store in FIFO for %s", type->name);
			continue;
		} else if (elements_left < 0) {
			LOG_ERR("Failed to get count for %s, error: %d", type->name, elements_left);
			continue;
		}

		while (elements_left > 0) {

			ret = k_mem_slab_alloc(type->slab, &chunk->data.ptr, K_NO_WAIT);
			if (ret) {
				LOG_ERR("Failed to allocate memory for stored data, error: %d",
					ret);

				return element_count_in_fifo;
			}

			LOG_DBG("FIFO slab allocated: %p", (void *)chunk);

			ret = backend->retrieve(type, chunk->data.ptr, type->data_size);
			if (ret < 0) {
				LOG_ERR("Failed to retrieve %s data, error: %d", type->name, ret);
				k_mem_slab_free(type->slab, (void *)chunk);

				return element_count_in_fifo;
			}

			chunk->type = type->data_type;
			chunk->finished = free_fifo_chunk;

			k_fifo_put(&storage_fifo, chunk);

			element_count_in_fifo++;

			elements_left--;
		}
	}

	return element_count_in_fifo;
}

static void fifo_purge(struct k_fifo *fifo)
{
	struct storage_data_chunk *chunk;

	LOG_DBG("Purging FIFO: %p", fifo);

	while ((chunk = k_fifo_get(fifo, K_NO_WAIT)) != NULL) {
		LOG_DBG("Purging chunk: %p", chunk);

		chunk->finished(chunk);
	}
}

static void storage_purge(void)
{
	int err;

	LOG_DBG("Purging storage");

	/* Purge the FIFO */
	fifo_purge(&storage_fifo);

	/* Clear all stored data */
	err = storage_backend_get()->clear();
	if (err) {
		LOG_ERR("Failed to clear storage backend, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void handle_storage_message(const struct storage_state *state_object)
{
	const struct storage_msg *msg = (const struct storage_msg *)state_object->msg_buf;

	LOG_DBG("Received message of type: %d", msg->type);

	if (msg->type == STORAGE_FLUSH) {
		flush_stored_data();
	}

	if (msg->type == STORAGE_FIFO_REQUEST) {
		int ret, err;
		struct storage_msg response_msg = {
			.type = STORAGE_FIFO_NOT_AVAILABLE,
		};

		/* Start populating the FIFO */
		ret = populate_fifo();
		if (ret < 0) {
			/* FIFO is not available */
			response_msg.type = STORAGE_FIFO_NOT_AVAILABLE;
		} else if (ret == 0) {
			/* FIFO is empty */
			response_msg.type = STORAGE_FIFO_EMPTY;
		} else {
			LOG_DBG("FIFO populated with %d records", ret);

			/* FIFO is available */
			response_msg.type = STORAGE_FIFO_AVAILABLE;
			response_msg.fifo = &storage_fifo;
			response_msg.data_len = ret;
		}

		err = zbus_chan_pub(&STORAGE_CHAN, &response_msg, K_SECONDS(1));
		if (err) {
			LOG_ERR("Failed to publish FIFO_NOT_AVAILABLE, error: %d", err);
			SEND_FATAL_ERROR();
		}
	}

	if (msg->type == STORAGE_PURGE) {
		/* Purge all stored data */
		storage_purge();

		return;
	}

	if (msg->type == STORAGE_FIFO_PURGE) {
		/* Purge the FIFO */
		fifo_purge(&storage_fifo);

		return;
	}
}

static void state_running_run(void *o)
{
	const struct storage_state *state_object = (const struct storage_state *)o;

	LOG_DBG("%s", __func__);

	if (state_object->chan == &STORAGE_CHAN) {
		handle_storage_message(state_object);

		return;
	}

	/* Check if message is from a registered data type */
	STRUCT_SECTION_FOREACH(storage_data, type) {
		if (state_object->chan == type->chan) {
			LOG_DBG("Chan: %p, chan name: %s", state_object->chan, state_object->chan->name);
			handle_data_message(type, state_object->msg_buf);

			return;
		}
	}
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

	err = zbus_chan_add_obs(&STORAGE_CHAN, &storage_subscriber, K_SECONDS(1));
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
