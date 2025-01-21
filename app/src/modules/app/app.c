/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <date_time.h>

#include "message_channel.h"

/* Register log module */
LOG_MODULE_REGISTER(app, CONFIG_APP_LOG_LEVEL);

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

static int app_init(void)
{
	/* Setup handler for date_time library */
	date_time_register_handler(date_time_handler);

	return 0;
}

SYS_INIT(app_init, POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY);
