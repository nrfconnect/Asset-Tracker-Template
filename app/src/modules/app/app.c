/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <date_time.h>
#include <net/nrf_cloud_coap.h>
#include <nrf_cloud_coap_transport.h>

#if defined(CONFIG_MEMFAULT)
#include <memfault/core/trace_event.h>
#endif /* CONFIG_MEMFAULT */

#include "message_channel.h"

/* Register log module */
LOG_MODULE_REGISTER(app, CONFIG_APP_LOG_LEVEL);

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(app);

/* Observe channels */
ZBUS_CHAN_ADD_OBS(TRIGGER_CHAN, app, 0);
ZBUS_CHAN_ADD_OBS(CLOUD_CHAN, app, 0);

#define MAX_MSG_SIZE (MAX(sizeof(enum trigger_type), sizeof(enum cloud_status)))

BUILD_ASSERT(CONFIG_APP_MODULE_WATCHDOG_TIMEOUT_SECONDS > CONFIG_APP_MODULE_EXEC_TIME_SECONDS_MAX,
	     "Watchdog timeout must be greater than maximum execution time");

static void shadow_get(bool delta_only)
{
	int err;
	uint8_t recv_buf[CONFIG_APP_MODULE_RECV_BUFFER_SIZE] = { 0 };
	size_t recv_buf_len = sizeof(recv_buf);

	LOG_DBG("Requesting device shadow from the device");

	err = nrf_cloud_coap_shadow_get(recv_buf, &recv_buf_len, delta_only,
					COAP_CONTENT_FORMAT_APP_JSON);
	if (err == -EACCES) {
		LOG_WRN("Not connected, error: %d", err);
		return;
	} else if (err == -ETIMEDOUT) {
		LOG_WRN("Request timed out, error: %d", err);
		return;
	} else if (err > 0) {
		LOG_WRN("Cloud error: %d", err);

		IF_ENABLED(CONFIG_MEMFAULT,
			(MEMFAULT_TRACE_EVENT_WITH_STATUS(nrf_cloud_coap_shadow_get, err)));

		return;
	} else if (err) {
		LOG_ERR("Failed to request shadow delta: %d", err);
		return;
	}

	/* No further processing of shadow is implemented */
}

static void task_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void date_time_handler(const struct date_time_evt *evt) {
	if (evt->type != DATE_TIME_NOT_OBTAINED) {
		int err;
		enum time_status time_status = TIME_AVAILABLE;

		err = zbus_chan_pub(&TIME_CHAN, &time_status, K_SECONDS(1));
		if (err) {
			LOG_ERR("zbus_chan_pub, error: %d", err);
			SEND_FATAL_ERROR();
		}
	}
}

static void app_task(void)
{
	int err;
	const struct zbus_channel *chan = NULL;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = (CONFIG_APP_MODULE_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms = (CONFIG_APP_MODULE_EXEC_TIME_SECONDS_MAX * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	uint8_t msg_buf[MAX_MSG_SIZE];

	LOG_DBG("Application module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, task_wdt_callback, (void *)k_current_get());

	/* Setup handler for date_time library */
	date_time_register_handler(date_time_handler);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = zbus_sub_wait_msg(&app, &chan, &msg_buf, zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		if (&CLOUD_CHAN == chan) {
			LOG_DBG("Cloud connection status received");

			const enum cloud_status *status = (const enum cloud_status *)msg_buf;

			if (*status == CLOUD_CONNECTED_READY_TO_SEND) {
				LOG_DBG("Cloud ready to send");

				shadow_get(false);
			}
		}

		if (&TRIGGER_CHAN == chan) {
			LOG_DBG("Trigger received");

			const enum trigger_type *type = (const enum trigger_type *)msg_buf;

			if (*type == TRIGGER_POLL) {
				LOG_DBG("Poll trigger received");

				shadow_get(true);
			}
		}
	}
}

K_THREAD_DEFINE(app_task_id,
		CONFIG_APP_MODULE_THREAD_STACK_SIZE,
		app_task, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

static int watchdog_init(void)
{
	__ASSERT((task_wdt_init(NULL) == 0), "Task watchdog init failure");

	return 0;
}

SYS_INIT(watchdog_init, POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY);
