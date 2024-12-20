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

#include "led_effect.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LED_1 1
#define LED_2 2
#define LED_3 3
#define LED_4 4

#define LED_ON(x)		(x)
#define LED_BLINK(x)		((x) << 8)
#define LED_GET_ON(x)		((x)&0xFF)
#define LED_GET_BLINK(x)	(((x) >> 8) & 0xFF)

#define LED_ON_PERIOD_NORMAL	500
#define LED_OFF_PERIOD_NORMAL	2000
#define LED_ON_PERIOD_ERROR	200
#define LED_OFF_PERIOD_ERROR	200
#define LED_ON_PERIOD_SHORT	350
#define LED_OFF_PERIOD_SHORT	350
#define LED_ON_PERIOD_STROBE	50
#define LED_OFF_PERIOD_STROBE	50
#define LED_OFF_PERIOD_LONG	4000

#define LED_MAX UINT8_MAX

#define LED_COLOR_OFF		LED_COLOR(0, 0, 0)
#define LED_COLOR_RED		LED_COLOR(LED_MAX, 0, 0)
#define LED_COLOR_GREEN		LED_COLOR(0, LED_MAX, 0)
#define LED_COLOR_BLUE		LED_COLOR(0, 0, LED_MAX)
#define LED_COLOR_YELLOW	LED_COLOR(LED_MAX, LED_MAX, 0)
#define LED_COLOR_CYAN		LED_COLOR(0, LED_MAX, LED_MAX)
#define LED_COLOR_PURPLE	LED_COLOR(LED_MAX, 0, LED_MAX)
#define LED_COLOR_WHITE		LED_COLOR(LED_MAX, LED_MAX, LED_MAX)

#define LED_LTE_CONNECTING_COLOR	LED_COLOR_YELLOW
#define LED_LOCATION_SEARCHING_COLOR	LED_COLOR_GREEN
#define LED_POLL_MODE_COLOR		LED_COLOR_BLUE
#define LED_ERROR_SYSTEM_FAULT_COLOR	LED_COLOR_RED
#define LED_OFF_COLOR			LED_COLOR_OFF

/**@brief LED state pattern definitions. */
enum led_state {
	LED_LTE_CONNECTING,
	LED_POLL_MODE,
	LED_LOCATION_SEARCHING,
	LED_ERROR_SYSTEM_FAULT,
	LED_ERROR_IRRECOVERABLE,
	LED_CONFIGURED,
	LED_OFF,
	LED_PATTERN_COUNT,
};

#ifdef __cplusplus
}
#endif

#endif /* LED_H__ */
