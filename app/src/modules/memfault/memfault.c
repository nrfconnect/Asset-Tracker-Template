/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/zbus/zbus.h>
#include "memfault/components.h"
#include <memfault/ports/zephyr/http.h>
#include <memfault/metrics/metrics.h>
#include <memfault/core/data_packetizer.h>
#include <memfault/core/trace_event.h>
#include "memfault/panics/coredump.h"
#include <modem/nrf_modem_lib_trace.h>
#include <modem/nrf_modem_lib.h>

#include "message_channel.h"

LOG_MODULE_REGISTER(memfault, CONFIG_APP_MEMFAULT_LOG_LEVEL);

/* Define listener for this module */
ZBUS_MSG_SUBSCRIBER_DEFINE(memfault);

/* Observe channels */
ZBUS_CHAN_ADD_OBS(CLOUD_CHAN, memfault, 0);

#define MAX_MSG_SIZE sizeof(enum cloud_status)

static void task_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

#if defined(CONFIG_APP_MEMFAULT_INCLUDE_MODEM_TRACES)

static const char *mimetypes[] = { MEMFAULT_CDR_BINARY };

static bool has_modem_traces;

static sMemfaultCdrMetadata trace_recording_metadata = {
	.start_time.type = kMemfaultCurrentTimeType_Unknown,
	.mimetypes = mimetypes,
	.num_mimetypes = 1,
	.data_size_bytes = 0,
	.collection_reason = "modem traces",
};

static int modem_trace_enable(void)
{
	int err;

	err = nrf_modem_lib_trace_level_set(NRF_MODEM_LIB_TRACE_LEVEL_LTE_AND_IP);
	if (err) {
		LOG_ERR("nrf_modem_lib_trace_level_set, error: %d", err);
		return err;
	}

	return 0;
}

/* Enable modem traces as soon as the modem is initialized if there is no valid coredump.
 * Modem traces are disabled by default via CONFIG_NRF_MODEM_LIB_TRACE_LEVEL_OFF.
 */
NRF_MODEM_LIB_ON_INIT(memfault_init_hook, on_modem_lib_init, NULL)

static void on_modem_lib_init(int ret, void *ctx)
{
	int err;

	if (memfault_coredump_has_valid_coredump(NULL)) {
		return;
	}

	err = nrf_modem_lib_trace_clear();
	if (err) {
		LOG_ERR("Failed to clear modem trace data: %d", err);
		return;
	}

	err = modem_trace_enable();
	if (err) {
		LOG_ERR("Failed to enable modem traces: %d", err);
		return;
	}
}

static bool has_cdr_cb(sMemfaultCdrMetadata *metadata)
{
	if (!has_modem_traces) {
		return false;
	}
	*metadata = trace_recording_metadata;
	return true;
}

static void mark_cdr_read_cb(void)
{
	has_modem_traces = false;
}

static bool read_data_cb(uint32_t offset, void *buf, size_t buf_len)
{
	ARG_UNUSED(offset);

	int err = nrf_modem_lib_trace_read(buf, buf_len);

	if (err == -ENODATA) {
		LOG_WRN("No more modem trace data to read");
		return false;
	} else if (err < 0) {
		LOG_ERR("Failed to read modem trace data: %d", err);
		return false;
	}

	return true;
}

static sMemfaultCdrSourceImpl s_my_custom_data_recording_source = {
	.has_cdr_cb = has_cdr_cb,
	.read_data_cb = read_data_cb,
	.mark_cdr_read_cb = mark_cdr_read_cb,
};

static void prepare_modem_trace_upload(void)
{
	size_t trace_size = 0;
	static bool memfault_cdr_source_registered;

	trace_size = nrf_modem_lib_trace_data_size();

	if (trace_size == -ENOTSUP) {
		LOG_ERR("The current modem trace backend is not supported");
		return;
	} else if (trace_size < 0) {
		LOG_ERR("Failed to get modem trace size: %d", trace_size);
		return;
	} else if (trace_size == 0) {
		LOG_DBG("No modem traces to send");
		return;
	}

	LOG_DBG("Preparing modem trace data upload of: %d bytes", trace_size);

	if (!memfault_cdr_source_registered) {
		memfault_cdr_register_source(&s_my_custom_data_recording_source);
		memfault_cdr_source_registered = true;
	}

	trace_recording_metadata.duration_ms = 0;
	trace_recording_metadata.data_size_bytes = trace_size;
	has_modem_traces = true;
}

#endif /* CONFIG_NRF_MODEM_LIB_TRACE && CONFIG_NRF_MODEM_LIB_TRACE_BACKEND_FLASH */

static void on_connected(void)
{
	bool has_coredump = memfault_coredump_has_valid_coredump(NULL);

	if (!has_coredump && !IS_ENABLED(CONFIG_APP_MEMFAULT_UPLOAD_METRICS_ON_CLOUD_READY)) {
		return;
	}

	/* Trigger collection of heartbeat data */
	memfault_metrics_heartbeat_debug_trigger();

	/* Check if there is any data available to be sent */
	if (!memfault_packetizer_data_available()) {
		return;
	}

#if defined(CONFIG_APP_MEMFAULT_INCLUDE_MODEM_TRACES)
	/* If there was a coredump, also send modem trace */

	if (has_coredump) {
		prepare_modem_trace_upload();
	}

#endif /* CONFIG_NRF_MODEM_LIB_TRACE && CONFIG_NRF_MODEM_LIB_TRACE_BACKEND_FLASH */

	memfault_zephyr_port_post_data();

#if defined(CONFIG_APP_MEMFAULT_INCLUDE_MODEM_TRACES)
	int err	= modem_trace_enable();

	if (err) {
		LOG_ERR("Failed to enable modem traces: %d", err);
		return;
	}
#endif /* CONFIG_NRF_MODEM_LIB_TRACE && CONFIG_NRF_MODEM_LIB_TRACE_BACKEND_FLASH */
}

void handle_cloud_chan(enum cloud_status status) {
	if (status == CLOUD_CONNECTED_READY_TO_SEND) {
		on_connected();
	}
}

void memfault_task(void)
{
	int err;
	const struct zbus_channel *chan;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = (CONFIG_APP_MEMFAULT_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_timeout = K_SECONDS(CONFIG_APP_MEMFAULT_ZBUS_TIMEOUT_SECONDS);
	uint8_t msg_buf[MAX_MSG_SIZE];

	LOG_DBG("Memfault module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, task_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		SEND_FATAL_ERROR();
		return;
	}

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("Failed to feed the watchdog: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = zbus_sub_wait_msg(&memfault, &chan, &msg_buf, zbus_timeout);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		if (&CLOUD_CHAN == chan) {
			LOG_DBG("Cloud status received");
			handle_cloud_chan(MSG_TO_CLOUD_STATUS(&msg_buf));
		}
	}
}

K_THREAD_DEFINE(memfault_module_tid, CONFIG_APP_MEMFAULT_THREAD_STACK_SIZE,
		memfault_task, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
