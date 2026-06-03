/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/smf.h>
#include <net/fota_download.h>
#include <memfault/ports/zephyr/fota.h>

#include "app_common.h"
#include "fota.h"

/* Register log module */
LOG_MODULE_REGISTER(fota, CONFIG_APP_FOTA_LOG_LEVEL);

BUILD_ASSERT(CONFIG_APP_FOTA_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_FOTA_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

/* Register message subscriber - will be called everytime a channel that the module listens on
 * receives a new message.
 */
ZBUS_MSG_SUBSCRIBER_DEFINE(fota);

/* Define FOTA channel */
ZBUS_CHAN_DEFINE(fota_chan,
		 struct fota_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Define the channels that the module subscribes to, their associated message types
 * and the subscriber that will receive the messages on the channel.
 */
#define CHANNEL_LIST(X)				\
	X(fota_chan,	struct fota_msg)	\

/* Calculate the maximum message size from the list of channels */
#define MAX_MSG_SIZE			MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

/* Add the fota subscriber as observer to all the channels in the list. */
#define ADD_OBSERVERS(_chan, _type)	ZBUS_CHAN_ADD_OBS(_chan, fota, 0);

/*
 * Expand to a call to ZBUS_CHAN_ADD_OBS for each channel in the list.
 * Example: ZBUS_CHAN_ADD_OBS(fota_chan, fota, 0);
 */
CHANNEL_LIST(ADD_OBSERVERS)

/* State machine */

/* FOTA module states */
enum fota_module_state {
	/* The module is initialized and running */
	STATE_RUNNING,
		/* The module is waiting for a poll request */
		STATE_WAITING_FOR_POLL_REQUEST,
		/* The module is polling for an update */
		STATE_POLLING_FOR_UPDATE,
		/* The module is downloading an update */
		STATE_DOWNLOADING_UPDATE,
		/* The module is waiting for the event FOTA_IMAGE_APPLY to apply the image */
		STATE_WAITING_FOR_IMAGE_APPLY,
		/* The module is applying the image */
		STATE_IMAGE_APPLYING,
		/* The FOTA module is waiting for a reboot */
		STATE_REBOOT_PENDING,
		/* The FOTA module is canceling the job */
		STATE_CANCELING,
};

/* State object.
 * Used to transfer context data between state changes.
 */
struct fota_state_object {
	/* This must be first */
	struct smf_ctx ctx;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;

	/* Buffer for last zbus message */
	uint8_t msg_buf[MAX_MSG_SIZE];
};

/* Forward declarations of state handlers */
static void state_running_entry(void *obj);
static enum smf_state_result state_running_run(void *obj);
static void state_waiting_for_poll_request_entry(void *obj);
static enum smf_state_result state_waiting_for_poll_request_run(void *obj);
static void state_polling_for_update_entry(void *obj);
static enum smf_state_result state_polling_for_update_run(void *obj);
static void state_downloading_update_entry(void *obj);
static enum smf_state_result state_downloading_update_run(void *obj);
static void state_waiting_for_image_apply_entry(void *obj);
static enum smf_state_result state_waiting_for_image_apply_run(void *obj);
static void state_image_applying_entry(void *obj);
static enum smf_state_result state_image_applying_run(void *obj);
static void state_reboot_pending_entry(void *obj);
static void state_canceling_entry(void *obj);
static enum smf_state_result state_canceling_run(void *obj);

static const struct smf_state states[] = {
	[STATE_RUNNING] =
		SMF_CREATE_STATE(state_running_entry,
				 state_running_run,
				 NULL,
				 NULL,	/* No parent state */
				 &states[STATE_WAITING_FOR_POLL_REQUEST]),
	[STATE_WAITING_FOR_POLL_REQUEST] =
		SMF_CREATE_STATE(state_waiting_for_poll_request_entry,
				 state_waiting_for_poll_request_run,
				 NULL,
				 &states[STATE_RUNNING],
				 NULL), /* No initial transition */
	[STATE_POLLING_FOR_UPDATE] =
		SMF_CREATE_STATE(state_polling_for_update_entry,
				 state_polling_for_update_run,
				 NULL,
				 &states[STATE_RUNNING],
				 NULL),
	[STATE_DOWNLOADING_UPDATE] =
		SMF_CREATE_STATE(state_downloading_update_entry,
				 state_downloading_update_run,
				 NULL,
				 &states[STATE_RUNNING],
				 NULL),
	[STATE_WAITING_FOR_IMAGE_APPLY] =
		SMF_CREATE_STATE(state_waiting_for_image_apply_entry,
				 state_waiting_for_image_apply_run,
				 NULL,
				 &states[STATE_RUNNING],
				 NULL),
	[STATE_IMAGE_APPLYING] =
		SMF_CREATE_STATE(state_image_applying_entry,
				 state_image_applying_run,
				 NULL,
				 &states[STATE_RUNNING],
				 NULL),
	[STATE_REBOOT_PENDING] =
		SMF_CREATE_STATE(state_reboot_pending_entry,
				 NULL,
				 NULL,
				 &states[STATE_RUNNING],
				 NULL),
	[STATE_CANCELING] =
		SMF_CREATE_STATE(state_canceling_entry,
				 state_canceling_run,
				 NULL,
				 &states[STATE_RUNNING],
				 NULL),
};

/* Custom Memfault FOTA download callback. Translates fota_download events into
 * the application's fota_chan event API.
 * Called from the download client thread while the fota module thread waits for messages.
 */
void memfault_fota_download_callback(const struct fota_download_evt *evt)
{
	int err;
	struct fota_msg fota_evt = { 0 };

	/* Track when the download has already started to report FOTA_DOWNLOADING_UPDATE
	 * only on the first progress event.
	 */
	static bool download_started;

	switch (evt->id) {
	case FOTA_DOWNLOAD_EVT_PROGRESS:
		if (!download_started) {
			download_started = true;
			fota_evt.type = FOTA_DOWNLOADING_UPDATE;
			break;
		}
		return;
	case FOTA_DOWNLOAD_EVT_FINISHED:
		download_started = false;
		LOG_INF("FOTA image downloaded successfully! Reboot request sent to apply update");
		fota_evt.type = FOTA_SUCCESS_REBOOT_NEEDED;
		break;
	case FOTA_DOWNLOAD_EVT_CANCELLED:
		download_started = false;
		LOG_WRN("FOTA download canceled");
		fota_evt.type = FOTA_DOWNLOAD_CANCELED;
		break;
	case FOTA_DOWNLOAD_EVT_ERROR:
		download_started = false;
		LOG_WRN("FOTA download error, cause: %d", evt->cause);
		if (evt->cause == FOTA_DOWNLOAD_ERROR_CAUSE_TYPE_MISMATCH) {
			fota_evt.type = FOTA_DOWNLOAD_REJECTED;
		} else {
			fota_evt.type = FOTA_DOWNLOAD_FAILED;
		}
		break;
	default:
		return;
	}

	err = zbus_chan_pub(&fota_chan, &fota_evt, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void fota_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

/* State handlers */

static void state_running_entry(void *obj)
{
	int err;
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	const struct fota_msg msg = { .type = FOTA_MODULE_READY };

	err = zbus_chan_pub(&fota_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static enum smf_state_result state_running_run(void *obj)
{
	struct fota_state_object const *state_object = obj;

	if (&fota_chan == state_object->chan) {
		const struct fota_msg *msg = (const struct fota_msg *)state_object->msg_buf;

		if (msg->type == FOTA_DOWNLOAD_CANCEL) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_CANCELING]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_waiting_for_poll_request_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);
}

static enum smf_state_result state_waiting_for_poll_request_run(void *obj)
{
	struct fota_state_object const *state_object = obj;

	if (&fota_chan == state_object->chan) {
		const struct fota_msg *msg =
			(const struct fota_msg *)state_object->msg_buf;

		if (msg->type == FOTA_POLL_REQUEST) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_POLLING_FOR_UPDATE]);

			return SMF_EVENT_HANDLED;
		} else if (msg->type == FOTA_DOWNLOAD_CANCEL) {
			LOG_DBG("No ongoing FOTA update, nothing to cancel");

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_polling_for_update_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	int err = memfault_zephyr_fota_start();

	if (err < 0) {
		LOG_ERR("memfault_zephyr_fota_start, error: %d", err);

		struct fota_msg evt = { .type = FOTA_DOWNLOAD_FAILED };

		err = zbus_chan_pub(&fota_chan, &evt, PUB_TIMEOUT);
		if (err) {
			LOG_ERR("zbus_chan_pub, error: %d", err);
			SEND_FATAL_ERROR();
		}

		return;
	} else if (err == 0) {
		LOG_DBG("No FOTA job available");

		struct fota_msg evt = { .type = FOTA_NO_AVAILABLE_UPDATE };

		err = zbus_chan_pub(&fota_chan, &evt, PUB_TIMEOUT);
		if (err) {
			LOG_ERR("zbus_chan_pub, error: %d", err);
			SEND_FATAL_ERROR();
		}

		return;
	}

	LOG_DBG("Job available, FOTA download started");
}

static enum smf_state_result state_polling_for_update_run(void *obj)
{
	struct fota_state_object const *state_object = obj;

	if (&fota_chan == state_object->chan) {
		const struct fota_msg *msg = (const struct fota_msg *)state_object->msg_buf;

		switch (msg->type) {
		case FOTA_DOWNLOADING_UPDATE:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DOWNLOADING_UPDATE]);

			return SMF_EVENT_HANDLED;
		case FOTA_NO_AVAILABLE_UPDATE:
			smf_set_state(SMF_CTX(state_object),
					      &states[STATE_WAITING_FOR_POLL_REQUEST]);

			return SMF_EVENT_HANDLED;
		case FOTA_DOWNLOAD_FAILED:
			smf_set_state(SMF_CTX(state_object),
					      &states[STATE_WAITING_FOR_POLL_REQUEST]);

			return SMF_EVENT_HANDLED;
		case FOTA_DOWNLOAD_CANCEL:
			LOG_DBG("No ongoing FOTA update, nothing to cancel");

			return SMF_EVENT_HANDLED;
		default:
			/* Don't care */
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* Erase the MCUboot trailer page (last 4 KiB of slot1 on external SPI-NOR).
 * stream_flash only erases written sectors, so a stale trailer survives J-Link
 * flashes of internal flash; boot_set_pending() then fails on the swap magic.
 */
static void erase_secondary_trailer(void)
{
	const struct flash_area *fa;
	int err = flash_area_open(PARTITION_ID(slot1_partition), &fa);

	if (err) {
		LOG_WRN("flash_area_open(slot1) failed: %d, skipping trailer erase", err);
		return;
	}

	const size_t page_size = CONFIG_SPI_NOR_FLASH_LAYOUT_PAGE_SIZE;
	const off_t trailer_off = fa->fa_size - page_size;

	err = flash_area_erase(fa, trailer_off, page_size);
	if (err) {
		LOG_WRN("flash_area_erase(slot1 trailer) failed: %d", err);
	} else {
		LOG_DBG("Erased slot1 trailer page at offset 0x%lx", (long)trailer_off);
	}

	flash_area_close(fa);
}

static void state_downloading_update_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	erase_secondary_trailer();
}

static enum smf_state_result state_downloading_update_run(void *obj)
{
	struct fota_state_object const *state_object = obj;

	if (&fota_chan == state_object->chan) {
		const struct fota_msg *msg = (const struct fota_msg *)state_object->msg_buf;

		switch (msg->type) {
		case FOTA_IMAGE_APPLY_NEEDED:
			smf_set_state(SMF_CTX(state_object),
					      &states[STATE_WAITING_FOR_IMAGE_APPLY]);

			return SMF_EVENT_HANDLED;
		case FOTA_SUCCESS_REBOOT_NEEDED:
			smf_set_state(SMF_CTX(state_object), &states[STATE_REBOOT_PENDING]);

			return SMF_EVENT_HANDLED;
		case FOTA_DOWNLOAD_CANCELED:
			__fallthrough;
		case FOTA_DOWNLOAD_REJECTED:
			__fallthrough;
		case FOTA_DOWNLOAD_FAILED:
			smf_set_state(SMF_CTX(state_object),
					      &states[STATE_WAITING_FOR_POLL_REQUEST]);

			return SMF_EVENT_HANDLED;
		default:
			/* Don't care */
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_waiting_for_image_apply_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);
}

static enum smf_state_result state_waiting_for_image_apply_run(void *obj)
{
	struct fota_state_object *state_object = obj;

	if (&fota_chan == state_object->chan) {
		const struct fota_msg *msg = (const struct fota_msg *)state_object->msg_buf;

		if (msg->type == FOTA_IMAGE_APPLY) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_IMAGE_APPLYING]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_image_applying_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	/* Modem FOTA image apply is not yet implemented for the Memfault backend. */
	LOG_WRN("Modem FOTA apply not yet implemented");
}

static enum smf_state_result state_image_applying_run(void *obj)
{
	struct fota_state_object const *state_object = obj;

	if (&fota_chan == state_object->chan) {
		const struct fota_msg *msg = (const struct fota_msg *)state_object->msg_buf;

		if (msg->type == FOTA_SUCCESS_REBOOT_NEEDED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_REBOOT_PENDING]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_reboot_pending_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("Waiting for the application to reboot in order to apply the update");
}

static void state_canceling_entry(void *obj)
{
	int err;

	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);
	LOG_DBG("Canceling download");

	err = fota_download_cancel();
	if (err) {
		LOG_ERR("fota_download_cancel, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static enum smf_state_result state_canceling_run(void *obj)
{
	struct fota_state_object const *state_object = obj;

	if (&fota_chan == state_object->chan) {
		const struct fota_msg *msg = (const struct fota_msg *)state_object->msg_buf;

		if (msg->type == FOTA_DOWNLOAD_CANCELED) {
			smf_set_state(SMF_CTX(state_object),
					      &states[STATE_WAITING_FOR_POLL_REQUEST]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void fota_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = (CONFIG_APP_FOTA_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_FOTA_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	static struct fota_state_object fota_state;

	LOG_DBG("FOTA module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, fota_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		SEND_FATAL_ERROR();
		return;
	}

	smf_set_initial(SMF_CTX(&fota_state), &states[STATE_RUNNING]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = zbus_sub_wait_msg(&fota, &fota_state.chan, fota_state.msg_buf, zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = smf_run_state(SMF_CTX(&fota_state));
		if (err) {
			LOG_ERR("smf_run_state(), error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}
	}
}

K_THREAD_DEFINE(fota_module_thread_id,
		CONFIG_APP_FOTA_THREAD_STACK_SIZE,
		fota_module_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
