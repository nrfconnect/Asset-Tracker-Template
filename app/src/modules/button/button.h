/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _BUTTON_H_
#define _BUTTON_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Button message types */
enum button_msg_type {
	/* Output message types */

	/** Short button press detected */
	BUTTON_PRESS_SHORT = 0x1,

	/** Long button press detected */
	BUTTON_PRESS_LONG,
};

/** @brief Button message data structure */
struct button_msg {
	enum button_msg_type type;
	uint8_t button_number;
};

/** @brief Cast a pointer to a message to a button message */
#define MSG_TO_BUTTON_MSG(_msg)	(*(const struct button_msg *)_msg)

/* Channels provided by this module */
ZBUS_CHAN_DECLARE(
	BUTTON_CHAN
);

#ifdef __cplusplus
}
#endif

#endif /* _BUTTON_H_ */
