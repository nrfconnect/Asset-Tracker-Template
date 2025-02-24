/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

 #include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/pm/device.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <date_time.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <modem/nrf_modem_lib_trace.h>
#include <net/nrf_cloud_defs.h>

#include "message_channel.h"
#include "button.h"
#include "cloud_module.h"

LOG_MODULE_REGISTER(shell, CONFIG_APP_SHELL_LOG_LEVEL);

BUILD_ASSERT(CONFIG_APP_SHELL_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_SHELL_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

#define PAYLOAD_MSG_TEMPLATE	\
	"{\""NRF_CLOUD_JSON_MSG_TYPE_KEY"\":\""NRF_CLOUD_JSON_MSG_TYPE_VAL_DATA"\","	\
	"\""NRF_CLOUD_JSON_APPID_KEY"\":\"%s\","					\
	"\""NRF_CLOUD_JSON_DATA_KEY"\":\"%s\","						\
	"\""NRF_CLOUD_MSG_TIMESTAMP_KEY"\":%lld}"

static const struct device *const shell_uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_shell_uart));
static const struct device *const uart1_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));

static void uart_disable_handler(struct k_work *work);
static void uart_enable_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(uart_disable_work, &uart_disable_handler);
static K_WORK_DELAYABLE_DEFINE(uart_enable_work, &uart_enable_handler);

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(shell);

enum zbus_test_type {
	PING,
};

ZBUS_CHAN_DEFINE(ZBUS_TEST_CHAN,
		 enum zbus_test_type,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(shell),
		 ZBUS_MSG_INIT(0)
);

#define MAX_MSG_SIZE (sizeof(enum zbus_test_type))

static bool uart_pm_enabled = IS_ENABLED(CONFIG_APP_SHELL_UART_PM_ENABLE);

static void uart_disable_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!uart_pm_enabled) {
		/* UART power management is disabled; do not disable UART */
		return;
	}

#ifdef CONFIG_NRF_MODEM_LIB_TRACE_BACKEND_UART
	int err = nrf_modem_lib_trace_level_set(NRF_MODEM_LIB_TRACE_LEVEL_OFF);
	if (err) {
		LOG_ERR("nrf_modem_lib_trace_level_set() failed with err = %d.", err);
	}
#endif

	/* Wait for UART buffers to be emptied before suspending.
	 * If a transfer is ongoing, the driver will cause an assertion to fail.
	 * 100 ms is an arbitrary value that should be enough for the buffers to empty.
	 */
	k_busy_wait(100 * USEC_PER_MSEC);

	if (device_is_ready(uart1_dev)) {
		pm_device_action_run(uart1_dev, PM_DEVICE_ACTION_SUSPEND);
	}

	if (device_is_ready(shell_uart_dev)) {
		pm_device_action_run(shell_uart_dev, PM_DEVICE_ACTION_SUSPEND);
	}
}

static void uart_enable_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (device_is_ready(shell_uart_dev)) {
		pm_device_action_run(shell_uart_dev, PM_DEVICE_ACTION_RESUME);
	}

	if (device_is_ready(uart1_dev)) {
		pm_device_action_run(uart1_dev, PM_DEVICE_ACTION_RESUME);
	}

#ifdef CONFIG_NRF_MODEM_LIB_TRACE_BACKEND_UART
	int err = nrf_modem_lib_trace_level_set(NRF_MODEM_LIB_TRACE_LEVEL_FULL);
	if (err) {
		LOG_ERR("nrf_modem_lib_trace_level_set() failed with err = %d.", err);
	}
#endif

	LOG_DBG("UARTs enabled\n");
}


static int cmd_uart_pm_enable(const struct shell *sh, size_t argc,
                         char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	uart_pm_enabled = true;
	shell_print(sh, "UART power management enabled");
	return 0;
}

static int cmd_uart_pm_disable(const struct shell *sh, size_t argc,
                         char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	uart_pm_enabled = false;
	shell_print(sh, "UART power management disabled");

	/* Ensure UARTs are enabled immediately for debugging */
	k_work_cancel_delayable(&uart_disable_work);
	k_work_schedule(&uart_enable_work, K_NO_WAIT);
	return 0;
}

static int cmd_uart_disable(const struct shell *sh, size_t argc,
                         char **argv)
{
	int sleep_time;

	if (argc != 2) {
		LOG_ERR("disable: invalid number of arguments");
		return -EINVAL;
	}

	sleep_time = atoi(argv[1]);
	if (sleep_time < 0) {
		LOG_ERR("disable: invalid sleep time");
		return -EINVAL;
	}

	if (sleep_time > 0) {
		shell_print(sh, "disable: disabling UARTs for %d seconds", sleep_time);
	} else {
		shell_print(sh, "disable: disabling UARTs indefinitely");
	}
	k_work_schedule(&uart_disable_work, K_NO_WAIT);

	if (sleep_time > 0) {
		k_work_schedule(&uart_enable_work, K_SECONDS(sleep_time));
	}

	return 0;
}

static int cmd_zbus_ping(const struct shell *sh, size_t argc,
                         char **argv)
{
	int err;
	enum zbus_test_type test_type;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	test_type = PING;

	err = zbus_chan_pub(&ZBUS_TEST_CHAN, &test_type, K_SECONDS(1));
	if (err) {
		shell_print(sh, "zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}

	return 0;
}

static int cmd_button_press(const struct shell *sh, size_t argc,
                         char **argv)
{
	int err;
	uint8_t button_number = 1;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	LOG_DBG("Button 1 pressed!");

	err = zbus_chan_pub(&BUTTON_CHAN, &button_number, K_SECONDS(1));
	if (err) {
			shell_print(sh, "zbus_chan_pub, error: %d", err);
			return 1;
	}

	return 0;
}

static int cmd_publish_on_payload_chan(const struct shell *sh, size_t argc, char **argv)
{
	int err;
	int ret;
	struct cloud_payload payload = {
		.buffer_len = strlen(argv[1]),
	};
	int64_t current_time;

	if (argc != 3) {
		shell_print(sh, "Invalid number of arguments (%d)", argc);
		shell_print(sh, "Usage: zbus publish payload_chan <appid> <data>");
		return 1;
	}

	err = date_time_now(&current_time);
	if (err) {
		shell_print(sh, "Failed to get current time, error: %d", err);
		return 1;
	}

	ret = snprintk(payload.buffer, sizeof(payload.buffer),
		PAYLOAD_MSG_TEMPLATE,
		argv[1], argv[2], current_time);
	if (ret < 0 || ret >= sizeof(payload.buffer)) {
		shell_print(sh, "Failed to format payload, error: %d", ret);
		return 1;
	}

	shell_print(sh, "Sending on payload channel: %s (%d bytes)",
		    payload.buffer, payload.buffer_len);

	err = zbus_chan_pub(&PAYLOAD_CHAN, &payload, K_SECONDS(1));
	if (err) {
		shell_print(sh, "zbus_chan_pub, error: %d", err);
		return 1;
	}

	return 0;
}


static void task_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}


/* Handle messages from the message queue.
 * Returns 0 if the message was handled successfully, otherwise an error code.
 */
static int handle_message(const struct zbus_channel *chan, uint8_t *msg_buf)
{
	if (&ZBUS_TEST_CHAN == chan) {
		enum zbus_test_type test_type = *(enum zbus_test_type *)msg_buf;

		if (test_type == PING) {
			LOG_INF("pong");
		}
	}

	return 0;
}

static void shell_task(void)
{
	int err;
	const struct zbus_channel *chan;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = (CONFIG_APP_SHELL_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_SHELL_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	uint8_t msg_buf[MAX_MSG_SIZE];

	LOG_DBG("Shell module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, task_wdt_callback, (void *)k_current_get());

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = zbus_sub_wait_msg(&shell, &chan, msg_buf, zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = handle_message(chan, msg_buf);
		if (err) {
			LOG_ERR("handle_message, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}
	}

}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_zbus_publish,
				SHELL_CMD(payload_chan,   NULL, "Publish on payload channel", cmd_publish_on_payload_chan),
				SHELL_SUBCMD_SET_END
		);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_zbus,
				SHELL_CMD(ping,   NULL, "Ping command.", cmd_zbus_ping),
				SHELL_CMD(button_press,   NULL, "Button press command.", cmd_button_press),
				SHELL_CMD(publish, &sub_zbus_publish, "Publish on a zbus channel", NULL),
				SHELL_SUBCMD_SET_END
		);

SHELL_CMD_REGISTER(zbus, &sub_zbus, "Zbus shell", NULL);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_uart,
				SHELL_CMD(disable, NULL, "<time in seconds>\nDisable UARTs for a given number of seconds. 0 means that "
										"UARTs remain disabled indefinitely.", cmd_uart_disable),
				SHELL_CMD(pm_enable, NULL, "Enable UART power management", cmd_uart_pm_enable),
				SHELL_CMD(pm_disable, NULL, "Disable UART power management", cmd_uart_pm_disable),
				SHELL_SUBCMD_SET_END
		);

SHELL_CMD_REGISTER(uart, &sub_uart, "UART shell", NULL);

K_THREAD_DEFINE(shell_task_id,
		CONFIG_APP_SHELL_THREAD_STACK_SIZE,
		shell_task, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
