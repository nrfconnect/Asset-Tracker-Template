/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/smf.h>

#include "app_common.h"
#include "button.h"
#include "network.h"
#include "cloud_module.h"
#include "fota.h"
#include "location.h"
#include "cbor_helper.h"

#if defined(CONFIG_APP_LED)
#include "led.h"
#endif /* CONFIG_APP_LED */

#if defined(CONFIG_APP_ENVIRONMENTAL)
#include "environmental.h"
#endif /* CONFIG_APP_ENVIRONMENTAL */

#if defined(CONFIG_APP_POWER)
#include "power.h"
#define POWER_MSG_SIZE	sizeof(struct power_msg)
#else
#define POWER_MSG_SIZE	0
#endif /* CONFIG_APP_POWER */

/* Register log module */
LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

#define MAX_MSG_SIZE	(MAX(sizeof(struct cloud_msg),						\
			 /* Button channel payload size */					\
			 MAX(sizeof(uint8_t),							\
			 /* Timer channel payload size */					\
			 MAX(sizeof(int),							\
			 MAX(sizeof(enum fota_msg_type),					\
			 MAX(sizeof(enum location_msg_type),					\
			 MAX(sizeof(struct network_msg), POWER_MSG_SIZE)))))))

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(main_subscriber);

ZBUS_CHAN_DEFINE(TIMER_CHAN,
		 int,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Observe channels */
ZBUS_CHAN_ADD_OBS(CLOUD_CHAN, main_subscriber, 0);
ZBUS_CHAN_ADD_OBS(BUTTON_CHAN, main_subscriber, 0);
ZBUS_CHAN_ADD_OBS(FOTA_CHAN, main_subscriber, 0);
ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, main_subscriber, 0);
ZBUS_CHAN_ADD_OBS(LOCATION_CHAN, main_subscriber, 0);
ZBUS_CHAN_ADD_OBS(TIMER_CHAN, main_subscriber, 0);

/* Forward declarations */
static void timer_work_fn(struct k_work *work);

/* Delayable work used to schedule triggers */
static K_WORK_DELAYABLE_DEFINE(trigger_work, timer_work_fn);

/* Forward declarations of state handlers */
static void running_entry(void *o);
static void running_run(void *o);

static void triggering_entry(void *o);
static void triggering_run(void *o);

static void requesting_location_entry(void *o);
static void requesting_location_run(void *o);

static void requesting_sensors_and_polling_entry(void *o);
static void requesting_sensors_and_polling_run(void *o);
static void requesting_sensors_and_polling_exit(void *o);

static void idle_entry(void *o);
static void idle_run(void *o);

static void fota_entry(void *o);
static void fota_run(void *o);

static void fota_downloading_run(void *o);

static void fota_waiting_for_network_disconnect_entry(void *o);
static void fota_waiting_for_network_disconnect_run(void *o);

static void fota_waiting_for_network_disconnect_to_apply_image_entry(void *o);
static void fota_waiting_for_network_disconnect_to_apply_image_run(void *o);

static void fota_applying_image_entry(void *o);
static void fota_applying_image_run(void *o);

static void fota_rebooting_entry(void *o);

enum state {
	/* Normal operation */
	STATE_RUNNING,
		/* Triggers are periodically sent at a configured interval */
		STATE_TRIGGERING,
			/* Requesting location from the location module */
			STATE_REQUESTING_LOCATION,
			/* Requesting sensor values and polling for downlink data */
			STATE_REQUESTING_SENSORS_AND_POLLING,
		/* Disconnected from the network, no triggers are sent */
		STATE_IDLE,
	/* Ongoing FOTA process, triggers are blocked */
	STATE_FOTA,
		/* FOTA image is being downloaded */
		STATE_FOTA_DOWNLOADING,
		/* Disconnecting from the network */
		STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT,
		/* Waiting for network disconnect to apply the image, state needed for
		 * Full Modem FOTA. Extra step needed to apply the image before rebooting.
		 */
		STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT_TO_APPLY_IMAGE,
		/* Applying the image */
		STATE_FOTA_APPLYING_IMAGE,
		/* Rebooting */
		STATE_FOTA_REBOOTING,
};

/* State object for the app module.
 * Used to transfer data between state changes.
 */
struct main_state {
	/* This must be first */
	struct smf_ctx ctx;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;

	/* Last received message */
	uint8_t msg_buf[MAX_MSG_SIZE];

	/* Trigger interval */
	uint64_t interval_sec;

	/* Cloud connection status */
	bool connected;
};

/* Construct state table */
static const struct smf_state states[] = {
	[STATE_RUNNING] = SMF_CREATE_STATE(
		running_entry,
		running_run,
		NULL,
		NULL,
		NULL
	),
	[STATE_TRIGGERING] = SMF_CREATE_STATE(
		triggering_entry,
		triggering_run,
		NULL,
		&states[STATE_RUNNING],
		&states[STATE_REQUESTING_LOCATION]
	),
	[STATE_REQUESTING_LOCATION] = SMF_CREATE_STATE(
		requesting_location_entry,
		requesting_location_run,
		NULL,
		&states[STATE_TRIGGERING],
		NULL
	),
	[STATE_REQUESTING_SENSORS_AND_POLLING] = SMF_CREATE_STATE(
		requesting_sensors_and_polling_entry,
		requesting_sensors_and_polling_run,
		requesting_sensors_and_polling_exit,
		&states[STATE_TRIGGERING],
		NULL
	),
	[STATE_IDLE] = SMF_CREATE_STATE(
		idle_entry,
		idle_run,
		NULL,
		&states[STATE_RUNNING],
		NULL
	),
	[STATE_FOTA] = SMF_CREATE_STATE(
		fota_entry,
		fota_run,
		NULL,
		NULL,
		&states[STATE_FOTA_DOWNLOADING]
	),
	[STATE_FOTA_DOWNLOADING] = SMF_CREATE_STATE(
		NULL,
		fota_downloading_run,
		NULL,
		&states[STATE_FOTA],
		NULL
	),
	[STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT] = SMF_CREATE_STATE(
		fota_waiting_for_network_disconnect_entry,
		fota_waiting_for_network_disconnect_run,
		NULL,
		&states[STATE_FOTA],
		NULL
	),
	[STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT_TO_APPLY_IMAGE] = SMF_CREATE_STATE(
		fota_waiting_for_network_disconnect_to_apply_image_entry,
		fota_waiting_for_network_disconnect_to_apply_image_run,
		NULL,
		&states[STATE_FOTA],
		NULL
	),
	[STATE_FOTA_APPLYING_IMAGE] = SMF_CREATE_STATE(
		fota_applying_image_entry,
		fota_applying_image_run,
		NULL,
		&states[STATE_FOTA],
		NULL
	),
	[STATE_FOTA_REBOOTING] = SMF_CREATE_STATE(
		fota_rebooting_entry,
		NULL,
		NULL,
		&states[STATE_FOTA],
		NULL
	),
};

/* Static helper function */

static void task_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void sensor_and_poll_triggers_send(void)
{
	int err;

#if defined(CONFIG_APP_REQUEST_NETWORK_QUALITY)
	struct network_msg network_msg = {
		.type = NETWORK_QUALITY_SAMPLE_REQUEST,
	};

	err = zbus_chan_pub(&NETWORK_CHAN, &network_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
#endif /* CONFIG_APP_REQUEST_NETWORK_QUALITY */

#if defined(CONFIG_APP_POWER)
	struct power_msg power_msg = {
		.type = POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST,
	};

	err = zbus_chan_pub(&POWER_CHAN, &power_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
#endif /* CONFIG_APP_POWER */

#if defined(CONFIG_APP_ENVIRONMENTAL)
	struct environmental_msg environmental_msg = {
		.type = ENVIRONMENTAL_SENSOR_SAMPLE_REQUEST,
	};

	err = zbus_chan_pub(&ENVIRONMENTAL_CHAN, &environmental_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
#endif /* CONFIG_APP_ENVIRONMENTAL */

	/* Send FOTA poll trigger */
	enum fota_msg_type fota_msg = FOTA_POLL_REQUEST;

	err = zbus_chan_pub(&FOTA_CHAN, &fota_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub FOTA trigger, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	/* Send trigger for shadow polling */
	struct cloud_msg cloud_msg = {
		.type = CLOUD_POLL_SHADOW
	};

	err = zbus_chan_pub(&CLOUD_CHAN, &cloud_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub shadow trigger, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

/* Delayable work used to send messages on the TIMER_CHAN */
static void timer_work_fn(struct k_work *work)
{
	int err;
	int dummy = 0;

	ARG_UNUSED(work);

	err = zbus_chan_pub(&TIMER_CHAN, &dummy, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

/* Zephyr State Machine framework handlers */

/* STATE_RUNNING */

static void running_entry(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	LOG_DBG("%s", __func__);

	if (state_object->connected) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_TRIGGERING]);
		return;
	}

	smf_set_state(SMF_CTX(state_object), &states[STATE_IDLE]);
}

static void running_run(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	if (state_object->chan == &FOTA_CHAN) {
		enum fota_msg_type msg = MSG_TO_FOTA_TYPE(state_object->msg_buf);

		if (msg == FOTA_DOWNLOADING_UPDATE) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_FOTA]);
			return;
		}
	}
}

/* STATE_IDLE */

static void idle_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

#if defined(CONFIG_APP_LED)
	int err;
	/* Blink Yellow */
	struct led_msg led_msg = {
		.type = LED_RGB_SET,
		.red = 255,
		.green = 255,
		.blue = 0,
		.duration_on_msec = 250,
		.duration_off_msec = 2000,
		.repetitions = 10,
	};

	err = zbus_chan_pub(&LED_CHAN, &led_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
#endif /* CONFIG_APP_LED */

	(void)k_work_cancel_delayable(&trigger_work);
}

static void idle_run(void *o)
{
	struct main_state *state_object = o;

	if (state_object->chan == &CLOUD_CHAN) {
		struct cloud_msg msg = MSG_TO_CLOUD_MSG(state_object->msg_buf);

		if (msg.type == CLOUD_CONNECTED_READY_TO_SEND) {
			state_object->connected = true;
			smf_set_state(SMF_CTX(state_object), &states[STATE_TRIGGERING]);
			return;
		}
	}
}

/* STATE_CONNECTED */

static void triggering_entry(void *o)
{
	int err;

	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

#if defined(CONFIG_APP_LED)
	/* Blink Green */
	struct led_msg led_msg = {
		.type = LED_RGB_SET,
		.red = 0,
		.green = 255,
		.blue = 0,
		.duration_on_msec = 250,
		.duration_off_msec = 2000,
		.repetitions = 10,
	};

	err = zbus_chan_pub(&LED_CHAN, &led_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
#endif /* CONFIG_APP_LED */

	err = k_work_reschedule(&trigger_work, K_NO_WAIT);
	if (err < 0) {
		LOG_ERR("k_work_reschedule, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

static void triggering_run(void *o)
{
	int err;
	struct main_state *state_object = o;

	if (state_object->chan == &CLOUD_CHAN) {
		struct cloud_msg msg = MSG_TO_CLOUD_MSG(state_object->msg_buf);

		if ((msg.type == CLOUD_CONNECTED_PAUSED) ||
		    (msg.type == CLOUD_DISCONNECTED)) {
			state_object->connected = false;
			smf_set_state(SMF_CTX(state_object), &states[STATE_IDLE]);
			return;
		}

		if (msg.type == CLOUD_SHADOW_RESPONSE) {
			err = get_update_interval_from_cbor_response(msg.response.buffer,
								     msg.response.buffer_data_len,
								     &state_object->interval_sec);
			if (err) {
				LOG_ERR("json_parse, error: %d", err);
				SEND_FATAL_ERROR();
				return;
			}

			LOG_WRN("Received new interval: %lld seconds", state_object->interval_sec);

			err = k_work_reschedule(&trigger_work,
						K_SECONDS(state_object->interval_sec));
			if (err < 0) {
				LOG_ERR("k_work_reschedule, error: %d", err);
				SEND_FATAL_ERROR();
			}
		}
	}
}

/* STATE_REQUESTING_LOCATION */

static void requesting_location_entry(void *o)
{
	int err;
	enum location_msg_type location_msg = LOCATION_SEARCH_TRIGGER;

	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	err = zbus_chan_pub(&LOCATION_CHAN, &location_msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub data sample trigger, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

static void requesting_location_run(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	if (state_object->chan == &LOCATION_CHAN) {
		enum location_msg_type msg = MSG_TO_LOCATION_TYPE(state_object->msg_buf);

		if (msg == LOCATION_SEARCH_DONE) {
			smf_set_state(SMF_CTX(state_object),
					      &states[STATE_REQUESTING_SENSORS_AND_POLLING]);
			return;
		}
	}

	if (state_object->chan == &BUTTON_CHAN) {
		smf_set_handled(SMF_CTX(state_object));
		return;
	}
}

/* STATE_REQUESTING_SENSORS_AND_POLLING */

static void requesting_sensors_and_polling_entry(void *o)
{
	int err;
	const struct main_state *state_object = (const struct main_state *)o;

	LOG_DBG("%s", __func__);

	sensor_and_poll_triggers_send();

	LOG_DBG("Next trigger in %lld seconds", state_object->interval_sec);

	err = k_work_reschedule(&trigger_work, K_SECONDS(state_object->interval_sec));
	if (err < 0) {
		LOG_ERR("k_work_reschedule, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void requesting_sensors_and_polling_run(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	if (state_object->chan == &TIMER_CHAN) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_REQUESTING_LOCATION]);
		return;
	}

	if (state_object->chan == &BUTTON_CHAN) {
		smf_set_state(SMF_CTX(state_object), &states[STATE_REQUESTING_LOCATION]);
		return;
	}
}

static void requesting_sensors_and_polling_exit(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	(void)k_work_cancel_delayable(&trigger_work);
}

/* STATE_FOTA */

static void fota_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	(void)k_work_cancel_delayable(&trigger_work);
}

static void fota_run(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	if (state_object->chan == &FOTA_CHAN) {
		enum fota_msg_type msg = MSG_TO_FOTA_TYPE(state_object->msg_buf);

		switch (msg) {
		case FOTA_DOWNLOAD_CANCELED:
			__fallthrough;
		case FOTA_DOWNLOAD_TIMED_OUT:
			__fallthrough;
		case FOTA_DOWNLOAD_FAILED:
			smf_set_state(SMF_CTX(state_object), &states[STATE_RUNNING]);
			return;
		default:
			/* Don't care */
			break;
		}
	}
}

/* STATE_FOTA_DOWNLOADING */

static void fota_downloading_run(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	if (state_object->chan == &FOTA_CHAN) {
		enum fota_msg_type msg = MSG_TO_FOTA_TYPE(state_object->msg_buf);

		switch (msg) {
		case FOTA_SUCCESS_REBOOT_NEEDED:
			smf_set_state(SMF_CTX(state_object),
					      &states[STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT]);
			return;
		case FOTA_IMAGE_APPLY_NEEDED:
			smf_set_state(SMF_CTX(state_object),
				&states[STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT_TO_APPLY_IMAGE]);
			return;
		default:
			/* Don't care */
			break;
		}
	}
}

/* STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT */

static void fota_waiting_for_network_disconnect_entry(void *o)
{
	int err;
	struct network_msg msg = {
		.type = NETWORK_DISCONNECT
	};

	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void fota_waiting_for_network_disconnect_run(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	if (state_object->chan == &NETWORK_CHAN) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		if (msg.type == NETWORK_DISCONNECTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_FOTA_REBOOTING]);
			return;
		}
	}
}

/* STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT_TO_APPLY_IMAGE */

static void fota_waiting_for_network_disconnect_to_apply_image_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	int err;
	struct network_msg msg = {
		.type = NETWORK_DISCONNECT
	};

	err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void fota_waiting_for_network_disconnect_to_apply_image_run(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	if (state_object->chan == &NETWORK_CHAN) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		if (msg.type == NETWORK_DISCONNECTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_FOTA_APPLYING_IMAGE]);
		}
	}
}

/* STATE_FOTA_APPLYING_IMAGE, */

static void fota_applying_image_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	int err;
	enum fota_msg_type msg = FOTA_IMAGE_APPLY;

	err = zbus_chan_pub(&FOTA_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void fota_applying_image_run(void *o)
{
	const struct main_state *state_object = (const struct main_state *)o;

	if (state_object->chan == &FOTA_CHAN) {
		enum fota_msg_type msg = MSG_TO_FOTA_TYPE(state_object->msg_buf);

		if (msg == FOTA_SUCCESS_REBOOT_NEEDED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_FOTA_REBOOTING]);
			return;
		}
	}
}

/* STATE_FOTA_REBOOTING */

static void fota_rebooting_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("%s", __func__);

	/* Reboot the device */
	LOG_WRN("Rebooting the device to apply the FOTA update");

	/* Flush log buffer */
	LOG_PANIC();

	k_sleep(K_SECONDS(5));

	sys_reboot(SYS_REBOOT_COLD);
}

int main(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = (CONFIG_APP_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	struct main_state main_state = { 0 };

	main_state.interval_sec = CONFIG_APP_MODULE_TRIGGER_TIMEOUT_SECONDS;

	LOG_DBG("Main has started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, task_wdt_callback, (void *)k_current_get());

	smf_set_initial(SMF_CTX(&main_state), &states[STATE_RUNNING]);

	while (1) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			SEND_FATAL_ERROR();

			return err;
		}

		err = zbus_sub_wait_msg(&main_subscriber, &main_state.chan, main_state.msg_buf,
					zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			SEND_FATAL_ERROR();

			return err;
		}

		err = smf_run_state(SMF_CTX(&main_state));
		if (err) {
			LOG_ERR("smf_run_state(), error: %d", err);
			SEND_FATAL_ERROR();

			return err;
		}
	}
}
