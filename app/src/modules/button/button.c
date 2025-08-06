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

/* Long press timeout in milliseconds */
#define LONG_PRESS_TIMEOUT_MS CONFIG_APP_BUTTON_LONG_PRESS_TIMEOUT_MS

/* Button state structure */
static struct {
	uint32_t pressed_buttons;
	struct k_work_delayable long_press_work;
} button_state;

/* Define channels provided by this module */
ZBUS_CHAN_DEFINE(BUTTON_CHAN,
		 struct button_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Work handler for long press detection */
static void long_press_work_handler(struct k_work *work)
{
	int err;
	struct button_msg msg;

	ARG_UNUSED(work);

	/* Check if button is still pressed */
	if (button_state.pressed_buttons & DK_BTN1_MSK) {
		LOG_DBG("Button 1 long press detected!");

		msg.button_number = 1;
		msg.type = BUTTON_PRESS_LONG;

		err = zbus_chan_pub(&BUTTON_CHAN, &msg, K_SECONDS(1));
		if (err) {
			LOG_ERR("zbus_chan_pub long press, error: %d", err);
			SEND_FATAL_ERROR();
		}
	}
}

/* Publish short press message */
static void publish_short_press(uint8_t button_number)
{
	int err;
	struct button_msg msg;

	msg.button_number = button_number;
	msg.type = BUTTON_PRESS_SHORT;

	LOG_DBG("Button %d short press", button_number);

	err = zbus_chan_pub(&BUTTON_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub short press, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

/* Button handler called when a user pushes a button */
static void button_handler(uint32_t button_states, uint32_t has_changed)
{
	/* Handle button 1 press */
	if (!(has_changed & DK_BTN1_MSK)) {
		return;
	}

	if (button_states & DK_BTN1_MSK) {
		button_state.pressed_buttons |= DK_BTN1_MSK;

		/* Start long press timer */
		k_work_schedule(&button_state.long_press_work, K_MSEC(LONG_PRESS_TIMEOUT_MS));
	} else {
		button_state.pressed_buttons &= ~DK_BTN1_MSK;

		/* Cancel long press timer if it was running and send short press */
		if (k_work_delayable_is_pending(&button_state.long_press_work)) {
			(void)k_work_cancel_delayable(&button_state.long_press_work);

			/* Timer was running, this is a short press */
			publish_short_press(1);
		}
	}
}

static int button_init(void)
{
	int err;

	LOG_DBG("button_init");

	/* Initialize button state */
	button_state.pressed_buttons = 0;

	k_work_init_delayable(&button_state.long_press_work,
			      long_press_work_handler);

	err = dk_buttons_init(button_handler);
	if (err) {
		LOG_ERR("dk_buttons_init, error: %d", err);
		SEND_FATAL_ERROR();
		return err;
	}

	return 0;
}

/* Initialize module at SYS_INIT() */
SYS_INIT(button_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
