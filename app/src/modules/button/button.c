/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <dk_buttons_and_leds.h>
#include <date_time.h>

#include "app_common.h"
#include "button.h"

/* Register log module */
LOG_MODULE_REGISTER(button, CONFIG_APP_BUTTON_LOG_LEVEL);

/* Define channels provided by this module */
ZBUS_CHAN_DEFINE(BUTTON_CHAN,
		 uint8_t,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Button handler called when a user pushes a button */
static void button_handler(uint32_t button_states, uint32_t has_changed)
{
	int err;
	uint8_t button_number = 1;

	if (has_changed & button_states & DK_BTN1_MSK) {
		LOG_DBG("Button 1 pressed!");

		err = zbus_chan_pub(&BUTTON_CHAN, &button_number, K_SECONDS(1));
		if (err) {
			LOG_ERR("zbus_chan_pub, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}
	}
}

static int button_init(void)
{
	LOG_DBG("button_init");

	int err = dk_buttons_init(button_handler);

	if (err) {
		LOG_ERR("dk_buttons_init, error: %d", err);
		SEND_FATAL_ERROR();
		return err;
	}

	return 0;
}

/* Initialize module at SYS_INIT() */
SYS_INIT(button_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
