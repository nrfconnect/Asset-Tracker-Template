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

#include "message_channel.h"
#include "button_object_encode.h"

/* Register log module */
LOG_MODULE_REGISTER(button, CONFIG_APP_BUTTON_LOG_LEVEL);

/* Encode and send button payload */
static void send_button_payload(void)
{
	int err;
	int64_t system_time;
	struct button_object button_object = { 0 };
	struct payload payload = { 0 };

	err = date_time_now(&system_time);
	if (err) {
		LOG_ERR("Failed to convert uptime to unix time, error: %d", err);
		return;
	}

	button_object.bt = (int32_t)(system_time / 1000);

	err = cbor_encode_button_object(payload.buffer, sizeof(payload.buffer),
				     &button_object, &payload.buffer_len);
	if (err) {
		LOG_ERR("Failed to encode button object, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	err = zbus_chan_pub(&PAYLOAD_CHAN, &payload, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

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
		send_button_payload();
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
