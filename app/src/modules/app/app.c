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
#include "app_object_decode.h"

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
	struct app_object app_object = { 0 };
	struct configuration configuration = { 0 };
	uint8_t buf_cbor[CONFIG_APP_MODULE_RECV_BUFFER_SIZE] = { 0 };
	size_t buf_cbor_len = sizeof(buf_cbor);
	size_t not_used;

	LOG_DBG("Requesting device configuration from the device shadow");

	err = nrf_cloud_coap_shadow_get(buf_cbor, &buf_cbor_len, delta_only,
					COAP_CONTENT_FORMAT_APP_CBOR);
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

	if (buf_cbor_len == 0) {
		LOG_DBG("No shadow delta changes available");
		return;
	}

	/* Workaroud: Sometimes nrf_cloud_coap_shadow_get() returns 0 even though obtaining
	 * the shadow failed. Ignore the payload if the first 10 bytes are zero.
	 */
	if (!memcmp(buf_cbor, "\0\0\0\0\0\0\0\0\0\0", 10)) {
		LOG_WRN("Returned buffer is empty, ignore");
		return;
	}

	err = cbor_decode_app_object(buf_cbor, buf_cbor_len, &app_object, &not_used);
	if (err) {
		/* Do not throw an error if decoding fails. This might occur if the shadow
 		 * structure or content changes. In such cases, we need to ensure the possibility
 		 * of performing a Firmware Over-The-Air (FOTA) update to address the issue.
 		 * Hardfaulting would prevent FOTA, hence it should be avoided.
 		 */
		LOG_ERR("Ignoring incoming configuration change due to decoding error: %d", err);
		LOG_HEXDUMP_ERR(buf_cbor, buf_cbor_len, "CBOR data");

		IF_ENABLED(CONFIG_MEMFAULT,
			(MEMFAULT_TRACE_EVENT_WITH_STATUS(cbor_decode_app_object, err)));

		return;
	}

	if (!app_object.lwm2m_present) {
		LOG_DBG("No LwM2M object present in shadow, ignoring");
		return;
	}

	if (app_object.lwm2m.lwm2m._1424010_present) {
		configuration.led_present = true;

		configuration.led_red = app_object.lwm2m.lwm2m._1424010._1424010._0._0._0;
		configuration.led_red_present =
			app_object.lwm2m.lwm2m._1424010._1424010._0._0_present;

		configuration.led_green = app_object.lwm2m.lwm2m._1424010._1424010._0._1._1;
		configuration.led_green_present =
			app_object.lwm2m.lwm2m._1424010._1424010._0._1_present;

		configuration.led_blue = app_object.lwm2m.lwm2m._1424010._1424010._0._2._2;
		configuration.led_blue_present =
			app_object.lwm2m.lwm2m._1424010._1424010._0._2_present;

		LOG_DBG("LED object (1424010) values received from cloud:");

		if (configuration.led_red_present) {
			LOG_DBG("New RED value: %d", configuration.led_red);
		}

		if (configuration.led_green_present) {
			LOG_DBG("New GREEN value: %d", configuration.led_green);
		}

		if (configuration.led_blue_present) {
			LOG_DBG("New BLUE value: %d", configuration.led_blue);
		}

		LOG_DBG("Timestamp: %lld", app_object.lwm2m.lwm2m._1424010._1424010._0._99);
	}

	if (app_object.lwm2m.lwm2m._1430110_present) {
		configuration.config_present = true;

		configuration.update_interval = app_object.lwm2m.lwm2m._1430110._1430110._0._0._0;
		configuration.update_interval_present =
			app_object.lwm2m.lwm2m._1430110._1430110._0._0_present;

		configuration.gnss = app_object.lwm2m.lwm2m._1430110._1430110._0._1._1;
		configuration.gnss_present = app_object.lwm2m.lwm2m._1430110._1430110._0._1_present;

		LOG_DBG("Application configuration object (1430110) values received from cloud:");

		if (configuration.update_interval_present) {
			LOG_DBG("New update interval: %lld", configuration.update_interval);
		}

		if (configuration.gnss_present) {
			LOG_DBG("New GNSS setting: %d", configuration.gnss);
		}

		LOG_DBG("Timestamp: %lld", app_object.lwm2m.lwm2m._1430110._1430110._0._99);
	}

	/* Distribute configuration */
	err = zbus_chan_pub(&CONFIG_CHAN, &configuration, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	/* Send the received configuration back to the reported shadow section. */
	err = nrf_cloud_coap_patch("state/reported", NULL, (uint8_t *)buf_cbor,
				   buf_cbor_len, COAP_CONTENT_FORMAT_APP_CBOR, true, NULL, NULL);
	if (err < 0) {
		LOG_ERR("Failed to send PATCH request: %d", err);
	} else if (err > 0) {
		LOG_ERR("Error from server: %d", err);
	}
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
