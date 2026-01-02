/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/zbus/zbus.h>
#include <net/nrf_cloud_coap.h>
#include <net/nrf_cloud_fota_poll.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <nrf_cloud_fota.h>
#include <zephyr/smf.h>
#include <net/fota_download.h>

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
ZBUS_CHAN_DEFINE(FOTA_CHAN,
		 enum fota_msg_type,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Observe channels */
ZBUS_CHAN_ADD_OBS(FOTA_CHAN, fota, 0);

#define MAX_MSG_SIZE sizeof(enum fota_msg_type)

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

	/* FOTA context */
	struct nrf_cloud_fota_poll_ctx fota_ctx;
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

/* FOTA support functions */

static void fota_reboot(enum nrf_cloud_fota_reboot_status status)
{
	int err;
	enum fota_msg_type evt = FOTA_SUCCESS_REBOOT_NEEDED;

	LOG_DBG("Reboot requested with FOTA status %d", status);

	err = zbus_chan_pub(&FOTA_CHAN, &evt, K_SECONDS(1));
	if (err) {
		LOG_DBG("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void fota_status(enum nrf_cloud_fota_status status, const char *const status_details)
{
	int err;
	enum fota_msg_type evt = { 0 };

	LOG_DBG("FOTA status: %d, details: %s", status, status_details ? status_details : "None");

	switch (status) {
	case NRF_CLOUD_FOTA_DOWNLOADING:
		LOG_DBG("Downloading firmware update");

		evt = FOTA_DOWNLOADING_UPDATE;
		break;
	case NRF_CLOUD_FOTA_FAILED:
		LOG_WRN("Firmware download failed");

		evt = FOTA_DOWNLOAD_FAILED;
		break;
	case NRF_CLOUD_FOTA_CANCELED:
		LOG_WRN("Firmware download canceled");

		evt = FOTA_DOWNLOAD_CANCELED;
		break;
	case NRF_CLOUD_FOTA_REJECTED:
		LOG_WRN("Firmware update rejected");

		evt = FOTA_DOWNLOAD_REJECTED;
		break;
	case NRF_CLOUD_FOTA_TIMED_OUT:
		LOG_WRN("Firmware download timed out");

		evt = FOTA_DOWNLOAD_TIMED_OUT;
		break;
	case NRF_CLOUD_FOTA_SUCCEEDED:
		LOG_DBG("Firmware update succeeded");
		LOG_DBG("Waiting for reboot request from the nRF Cloud FOTA Poll library");

		/* Don't send any event in case of success, the nRF Cloud FOTA poll library will
		 * notify when a reboot is needed via the fota_reboot() callback.
		 */
		return;
	case NRF_CLOUD_FOTA_FMFU_VALIDATION_NEEDED:
		LOG_DBG("Full Modem FOTA Update validation needed, network disconnect required");

		evt = FOTA_IMAGE_APPLY_NEEDED;
		break;
	default:
		LOG_DBG("Unknown FOTA status: %d", status);
		return;
	}

	err = zbus_chan_pub(&FOTA_CHAN, &evt, K_SECONDS(1));
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
	struct fota_state_object *state_object = obj;

	LOG_DBG("%s", __func__);

	/* Initialize the FOTA context */
	err = nrf_cloud_fota_poll_init(&state_object->fota_ctx);
	if (err) {
		LOG_ERR("nrf_cloud_fota_poll_init failed: %d", err);
		SEND_FATAL_ERROR();
	}

	/* Process pending FOTA job, the FOTA type is returned */
	err = nrf_cloud_fota_poll_process_pending(&state_object->fota_ctx);
	if (err < 0) {
		LOG_ERR("nrf_cloud_fota_poll_process_pending failed: %d", err);
		SEND_FATAL_ERROR();
	}
}

static enum smf_state_result state_running_run(void *obj)
{
	struct fota_state_object const *state_object = obj;

	if (&FOTA_CHAN == state_object->chan) {
		const enum fota_msg_type msg_type = MSG_TO_FOTA_TYPE(state_object->msg_buf);

		if (msg_type == FOTA_DOWNLOAD_CANCEL) {
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

	if (&FOTA_CHAN == state_object->chan) {
		const enum fota_msg_type msg_type = MSG_TO_FOTA_TYPE(state_object->msg_buf);

		if (msg_type == FOTA_POLL_REQUEST) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_POLLING_FOR_UPDATE]);

			return SMF_EVENT_HANDLED;
		} else if (msg_type == FOTA_DOWNLOAD_CANCEL) {
			LOG_DBG("No ongoing FOTA update, nothing to cancel");

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_polling_for_update_entry(void *obj)
{
	struct fota_state_object *state_object = obj;

	LOG_DBG("%s", __func__);

	/* Start the FOTA processing */
	int err = nrf_cloud_fota_poll_process(&state_object->fota_ctx);

	if (err == -EINVAL) {
		LOG_DBG("nrf_cloud_fota_poll_process, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	} else if (err) {
		LOG_DBG("No FOTA job available");

		enum fota_msg_type evt = FOTA_NO_AVAILABLE_UPDATE;

		err = zbus_chan_pub(&FOTA_CHAN, &evt, K_SECONDS(1));
		if (err) {
			LOG_ERR("zbus_chan_pub, error: %d", err);
			SEND_FATAL_ERROR();
		}

		return;
	}

	LOG_DBG("Job available, FOTA processing started");
}

static enum smf_state_result state_polling_for_update_run(void *obj)
{
	struct fota_state_object const *state_object = obj;

	if (&FOTA_CHAN == state_object->chan) {
		const enum fota_msg_type evt = MSG_TO_FOTA_TYPE(state_object->msg_buf);

		switch (evt) {
		case FOTA_DOWNLOADING_UPDATE:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DOWNLOADING_UPDATE]);

			return SMF_EVENT_HANDLED;
		case FOTA_NO_AVAILABLE_UPDATE:
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

static void state_downloading_update_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);
}

static enum smf_state_result state_downloading_update_run(void *obj)
{
	struct fota_state_object const *state_object = obj;

	if (&FOTA_CHAN == state_object->chan) {
		const enum fota_msg_type evt = MSG_TO_FOTA_TYPE(state_object->msg_buf);

		switch (evt) {
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
		case FOTA_DOWNLOAD_TIMED_OUT:
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

	if (&FOTA_CHAN == state_object->chan) {
		const enum fota_msg_type evt = MSG_TO_FOTA_TYPE(state_object->msg_buf);

		if (evt == FOTA_IMAGE_APPLY) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_IMAGE_APPLYING]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_image_applying_entry(void *obj)
{
	struct fota_state_object *state_object = obj;

	LOG_DBG("Applying downloaded firmware image");

	/* Apply the downloaded firmware image */
	int err = nrf_cloud_fota_poll_update_apply(&state_object->fota_ctx);

	if (err) {
		LOG_ERR("nrf_cloud_fota_poll_update_apply, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static enum smf_state_result state_image_applying_run(void *obj)
{
	struct fota_state_object const *state_object = obj;

	if (&FOTA_CHAN == state_object->chan) {
		const enum fota_msg_type evt = MSG_TO_FOTA_TYPE(state_object->msg_buf);

		if (evt == FOTA_SUCCESS_REBOOT_NEEDED) {
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

	if (&FOTA_CHAN == state_object->chan) {
		const enum fota_msg_type msg = MSG_TO_FOTA_TYPE(state_object->msg_buf);

		if (msg == FOTA_DOWNLOAD_CANCELED) {
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
	struct fota_state_object fota_state = {
		.fota_ctx.reboot_fn = fota_reboot,
		.fota_ctx.status_fn = fota_status,
	};

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
