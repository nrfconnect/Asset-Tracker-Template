/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**@file
 *
 * @brief   LED module.
 *
 * Module that handles LED behaviour.
 */

#ifndef LED_H__
#define LED_H__

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Channels provided by this module */
ZBUS_CHAN_DECLARE(
	LED_CHAN
);

enum led_msg_type {
	LED_RGB_SET,
};

struct led_msg {
	enum led_msg_type type;

	/** RGB values (0 to 255) */
	uint8_t red;
	uint8_t green;
	uint8_t blue;

	/** Duration of the RGB on/off cycle */
	uint32_t duration_on_msec;
	uint32_t duration_off_msec;

	/** Number of on/off cycles (-1 indicates forever) */
	int repetitions;
};

#define MSG_TO_LED_MSG(_msg) ((const struct led_msg *)_msg)

#ifdef __cplusplus
}
#endif

#endif /* LED_H__ */
