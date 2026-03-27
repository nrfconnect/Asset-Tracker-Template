/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/smf.h>
#include <net/fota_download.h>
#include <modem/nrf_modem_lib.h>

#if defined(CONFIG_APP_FOTA_TYPE_NRF_CLOUD)
#include <net/nrf_cloud_fota_poll.h>
#include <nrf_cloud_fota.h>
#elif defined(CONFIG_APP_FOTA_TYPE_MEMFAULT)
#include <memfault/nrfconnect_port/fota.h>
#endif

#include <zephyr/task_wdt/task_wdt.h>

#include "app_common.h"
#ifdef CONFIG_APP_INSPECT_SHELL
#include "app_inspect.h"
#endif /* CONFIG_APP_INSPECT_SHELL */
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

/* Private channel message types for internal state management. */
enum priv_fota_msg_type {
	/* Modem has completed initialization. */
	FOTA_PRIV_MODEM_INITIALIZED,
};

struct priv_fota_msg {
	enum priv_fota_msg_type type;
};

/* Create private fota channel for internal messaging that is not intended for external use. */
ZBUS_CHAN_DEFINE(priv_fota_chan,
		 struct priv_fota_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Define the channels that the module subscribes to, their associated message types
 * and the subscriber that will receive the messages on the channel.
 */
#define CHANNEL_LIST(X)							\
	X(fota_chan,		struct fota_msg)				\
	X(priv_fota_chan,	struct priv_fota_msg)			\

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
		/* The module is waiting for modem initialization */
		STATE_WAITING_FOR_MODEM_INIT,
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

	#if defined(CONFIG_APP_FOTA_TYPE_NRF_CLOUD)
	/* FOTA context */
	struct nrf_cloud_fota_poll_ctx fota_ctx;
	#endif
};

/* Forward declarations of state handlers */
static void state_running_entry(void *obj);
static enum smf_state_result state_running_run(void *obj);
static void state_waiting_for_modem_init_entry(void *obj);
static enum smf_state_result state_waiting_for_modem_init_run(void *obj);
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
				 &states[STATE_WAITING_FOR_MODEM_INIT]),
	[STATE_WAITING_FOR_MODEM_INIT] =
		SMF_CREATE_STATE(state_waiting_for_modem_init_entry,
				 state_waiting_for_modem_init_run,
				 NULL,
				 &states[STATE_RUNNING],
				 NULL), /* No initial transition */
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

#if defined(CONFIG_APP_INSPECT_SHELL)
static struct fota_state_object *fota_state_ctx;

static const char *fota_state_to_string(enum fota_module_state state)
{
	switch (state) {
	case STATE_RUNNING:
		return "STATE_RUNNING";
	case STATE_WAITING_FOR_MODEM_INIT:
		return "STATE_WAITING_FOR_MODEM_INIT";
	case STATE_WAITING_FOR_POLL_REQUEST:
		return "STATE_WAITING_FOR_POLL_REQUEST";
	case STATE_POLLING_FOR_UPDATE:
		return "STATE_POLLING_FOR_UPDATE";
	case STATE_DOWNLOADING_UPDATE:
		return "STATE_DOWNLOADING_UPDATE";
	case STATE_WAITING_FOR_IMAGE_APPLY:
		return "STATE_WAITING_FOR_IMAGE_APPLY";
	case STATE_IMAGE_APPLYING:
		return "STATE_IMAGE_APPLYING";
	case STATE_REBOOT_PENDING:
		return "STATE_REBOOT_PENDING";
	case STATE_CANCELING:
		return "STATE_CANCELING";
	default:
		return "STATE_UNKNOWN";
	}
}

APP_INSPECT_MODULE_REGISTER_STATE(fota,
				  fota_state_ctx,
				  states,
				  enum fota_module_state,
				  fota_state_to_string);
#endif /* CONFIG_APP_INSPECT_SHELL */

static void on_modem_init(int ret, void *ctx)
{
	int err;
	struct priv_fota_msg msg = { .type = FOTA_PRIV_MODEM_INITIALIZED };

	ARG_UNUSED(ctx);

	if (ret) {
		LOG_ERR("Modem init failed: %d, fota module cannot initialize", ret);

		return;
	}

	err = zbus_chan_pub(&priv_fota_chan, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}
}

NRF_MODEM_LIB_ON_INIT(fota_modem_init_hook, on_modem_init, NULL);

/* FOTA support functions */

static void fota_publish_event(enum fota_msg_type evt)
{
	int err;
	struct fota_msg evt = { .type = FOTA_SUCCESS_REBOOT_NEEDED };

	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

#if defined(CONFIG_APP_FOTA_TYPE_MEMFAULT)
void memfault_fota_download_callback(const struct fota_download_evt *evt)
{
	enum fota_msg_type app_evt;

	switch (evt->id) {
	case FOTA_DOWNLOAD_EVT_FINISHED:
		LOG_DBG("Memfault FOTA download completed, reboot required");
		app_evt = FOTA_SUCCESS_REBOOT_NEEDED;
		break;
	case FOTA_DOWNLOAD_EVT_CANCELLED:
		LOG_WRN("Memfault FOTA download canceled");
		app_evt = FOTA_DOWNLOAD_CANCELED;
		break;
	case FOTA_DOWNLOAD_EVT_ERROR:
		switch (evt->cause) {
		case FOTA_DOWNLOAD_ERROR_CAUSE_INVALID_UPDATE:
		case FOTA_DOWNLOAD_ERROR_CAUSE_TYPE_MISMATCH:
			LOG_WRN("Memfault FOTA update rejected, cause: %d", evt->cause);
			app_evt = FOTA_DOWNLOAD_REJECTED;
			break;
		default:
			LOG_WRN("Memfault FOTA download failed, cause: %d", evt->cause);
			app_evt = FOTA_DOWNLOAD_FAILED;
			break;
		}
		break;
	default:
		return;
	}

	fota_publish_event(app_evt);
}
#endif

#if defined(CONFIG_APP_FOTA_TYPE_NRF_CLOUD)
static void fota_reboot(enum nrf_cloud_fota_reboot_status status)
{
	enum fota_msg_type evt = FOTA_SUCCESS_REBOOT_NEEDED;

	LOG_DBG("Reboot requested with FOTA status %d", status);

	fota_publish_event(evt);
}

static void fota_status(enum nrf_cloud_fota_status status, const char *const status_details)
{
	int err;
	struct fota_msg evt = { 0 };

	LOG_DBG("FOTA status: %d, details: %s", status, status_details ? status_details : "None");

	switch (status) {
	case NRF_CLOUD_FOTA_DOWNLOADING:
		LOG_DBG("Downloading firmware update");

		evt.type = FOTA_DOWNLOADING_UPDATE;
		break;
	case NRF_CLOUD_FOTA_FAILED:
		LOG_WRN("Firmware download failed");

		evt.type = FOTA_DOWNLOAD_FAILED;
		break;
	case NRF_CLOUD_FOTA_CANCELED:
		LOG_WRN("Firmware download canceled");

		evt.type = FOTA_DOWNLOAD_CANCELED;
		break;
	case NRF_CLOUD_FOTA_REJECTED:
		LOG_WRN("Firmware update rejected");

		evt.type = FOTA_DOWNLOAD_REJECTED;
		break;
	case NRF_CLOUD_FOTA_TIMED_OUT:
		LOG_WRN("Firmware download timed out");

		evt.type = FOTA_DOWNLOAD_TIMED_OUT;
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

		evt.type = FOTA_IMAGE_APPLY_NEEDED;
		break;
	default:
		LOG_DBG("Unknown FOTA status: %d", status);
		return;
	}

	fota_publish_event(evt);
}
#endif

static void fota_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

/* State handlers */

static void state_running_entry(void *obj)
{
	#if defined(CONFIG_APP_FOTA_TYPE_NRF_CLOUD)
	int err;
	struct fota_state_object *state_object = obj;
	#endif

	LOG_DBG("%s", __func__);

	#if defined(CONFIG_APP_FOTA_TYPE_NRF_CLOUD)
	/* Initialize the FOTA context */
	err = nrf_cloud_fota_poll_init(&state_object->fota_ctx);
	if (err) {
		LOG_ERR("nrf_cloud_fota_poll_init failed: %d", err);
		SEND_FATAL_ERROR();
	}
	#else
	ARG_UNUSED(obj);
	#endif
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

static void state_waiting_for_modem_init_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);
	LOG_DBG("Waiting for modem initialization before processing pending FOTA job");
}

static enum smf_state_result state_waiting_for_modem_init_run(void *obj)
{
	struct fota_state_object *state_object = obj;

	if (&priv_fota_chan == state_object->chan) {
		const struct priv_fota_msg *msg =
			(const struct priv_fota_msg *)state_object->msg_buf;

		/* Wait for modem initialization to complete before processing pending FOTA job.
		 * This ensures the modem DFU result callback has been invoked.
		 */
		if (msg->type == FOTA_PRIV_MODEM_INITIALIZED) {
			LOG_DBG("Modem initialized, processing pending FOTA job");

			#if defined(CONFIG_APP_FOTA_TYPE_NRF_CLOUD)
			int err = nrf_cloud_fota_poll_process_pending(&state_object->fota_ctx);

			if (err < 0) {
				LOG_ERR("nrf_cloud_fota_poll_process_pending failed: %d", err);
				SEND_FATAL_ERROR();
			}
			#endif

			const struct fota_msg msg_out = { .type = FOTA_MODULE_READY };

			err = zbus_chan_pub(&fota_chan, &msg_out, PUB_TIMEOUT);
			if (err) {
				LOG_ERR("zbus_chan_pub, error: %d", err);
				SEND_FATAL_ERROR();
			}

			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_WAITING_FOR_POLL_REQUEST]);

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
	#if defined(CONFIG_APP_FOTA_TYPE_NRF_CLOUD)
	struct fota_state_object *state_object = obj;
	#endif
	int err;

	LOG_DBG("%s", __func__);

	#if defined(CONFIG_APP_FOTA_TYPE_NRF_CLOUD)
	/* Start the FOTA processing */
	err = nrf_cloud_fota_poll_process(&state_object->fota_ctx);

	if (err == -EINVAL) {
		LOG_DBG("nrf_cloud_fota_poll_process, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	} else if (err) {
		LOG_DBG("No FOTA job available");

		struct fota_msg evt = { .type = FOTA_NO_AVAILABLE_UPDATE };

		err = zbus_chan_pub(&fota_chan, &evt, PUB_TIMEOUT);
		if (err) {
			LOG_ERR("zbus_chan_pub, error: %d", err);
			SEND_FATAL_ERROR();
		}

		return;
	}

	LOG_DBG("Job available, FOTA processing started");
	#else
	ARG_UNUSED(obj);

	err = memfault_fota_start();
	if (err < 0) {
		LOG_WRN("memfault_fota_start failed: %d", err);
		fota_publish_event(FOTA_DOWNLOAD_FAILED);
		return;
	}

	if (err == 0) {
		LOG_DBG("No Memfault FOTA job available");
		fota_publish_event(FOTA_NO_AVAILABLE_UPDATE);
		return;
	}

	LOG_DBG("Memfault FOTA job available, download started");
	fota_publish_event(FOTA_DOWNLOADING_UPDATE);
	#endif
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
		case FOTA_DOWNLOAD_FAILED:
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_WAITING_FOR_POLL_REQUEST]);

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
	#if defined(CONFIG_APP_FOTA_TYPE_NRF_CLOUD)
	struct fota_state_object *state_object = obj;

	LOG_DBG("Applying downloaded firmware image");

	/* Apply the downloaded firmware image */
	int err = nrf_cloud_fota_poll_update_apply(&state_object->fota_ctx);

	if (err) {
		LOG_ERR("nrf_cloud_fota_poll_update_apply, error: %d", err);
		SEND_FATAL_ERROR();
	}
	#else
	ARG_UNUSED(obj);
	LOG_ERR("Image apply requested for unsupported FOTA backend");
	SEND_FATAL_ERROR();
	#endif
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
	static struct fota_state_object fota_state = {
		#if defined(CONFIG_APP_FOTA_TYPE_NRF_CLOUD)
		.fota_ctx.reboot_fn = fota_reboot,
		.fota_ctx.status_fn = fota_status,
		#endif
	};

#if defined(CONFIG_APP_INSPECT_SHELL)
	fota_state_ctx = &fota_state;
#endif /* CONFIG_APP_INSPECT_SHELL */

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
