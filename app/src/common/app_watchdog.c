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

/* nRF91 labels its watchdog "wdt". Other SoCs, such as nRF9251, have several
 * watchdog instances with SoC-specific labels and select one with the standard
 * watchdog0 alias. Prefer the label so nRF91 keeps using the same instance.
 */
#if DT_NODE_EXISTS(DT_NODELABEL(wdt))
#define APP_WATCHDOG_NODE DT_NODELABEL(wdt)
#elif DT_NODE_EXISTS(DT_ALIAS(watchdog0))
#define APP_WATCHDOG_NODE DT_ALIAS(watchdog0)
#else
#error "No hardware watchdog: expected a wdt node label or a watchdog0 alias"
#endif

static int app_watchdog_init(void)
{
	const struct device *const hw_wdt = DEVICE_DT_GET(APP_WATCHDOG_NODE);
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
