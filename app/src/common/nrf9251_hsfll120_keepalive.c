/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/* Temporary workaround for NRFX-10395: HSFLL120 returns from SystemOnAllIdle at the wrong
 * frequency, about 14% slow. Everything in that clock domain is affected, including SPI120 to
 * the external flash and UARTE120. TIMER120 is started at boot and left running, which keeps the
 * domain out of the all-idle state and the faulty transition from happening.
 *
 * Remove together with
 * patches/0001-zephyr-gpio-nrfx-disable-pin-retention-nrf9251-workaround.patch once the clock
 * issue is fixed.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#define TIMER120_NODE DT_NODELABEL(timer120)

BUILD_ASSERT(DT_NODE_HAS_STATUS(TIMER120_NODE, okay),
	     "timer120 must be enabled in devicetree to keep HSFLL120 active");

LOG_MODULE_REGISTER(hsfll120_keepalive, CONFIG_APP_LOG_LEVEL);

/* Start TIMER120 and leave it running for the lifetime of the application.
 *
 * Returns 0 on success, -ENODEV if the timer device is not ready, or the error returned by
 * counter_start().
 */
static int hsfll120_keepalive_init(void)
{
	const struct device *const timer = DEVICE_DT_GET(TIMER120_NODE);
	int err;

	if (!device_is_ready(timer)) {
		LOG_ERR("TIMER120 is not ready");
		return -ENODEV;
	}

	err = counter_start(timer);
	if (err) {
		LOG_ERR("counter_start, error: %d", err);
		return err;
	}

	LOG_DBG("TIMER120 started, HSFLL120 kept out of all-idle");

	return 0;
}

SYS_INIT(hsfll120_keepalive_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
