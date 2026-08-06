/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/task_wdt/task_wdt.h>

#include "app_common.h"

LOG_MODULE_REGISTER(app_watchdog, CONFIG_APP_LOG_LEVEL);

static int app_watchdog_init(void)
{
	const struct device *const hw_wdt = DEVICE_DT_GET(DT_NODELABEL(wdt));
	int err;

	if (!device_is_ready(hw_wdt)) {
		LOG_ERR("Hardware watchdog not ready");
		SEND_FATAL_ERROR();

		return -ENODEV;
	}

	err = task_wdt_init(hw_wdt);
	if (err) {
		LOG_ERR("task_wdt_init failed: %d", err);
		SEND_FATAL_ERROR();

		return err;
	}

	LOG_DBG("Task watchdog initialized with hardware watchdog fallback");

	return 0;
}

SYS_INIT(app_watchdog_init, POST_KERNEL, 0);
