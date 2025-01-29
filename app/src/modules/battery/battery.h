/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _BATTERY_H_
#define _BATTERY_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Channels provided by this module */
ZBUS_CHAN_DECLARE(
	BATTERY_CHAN
);

enum battery_msg_type {
	/* Output message types */

	/* Response message to a request for a battery percentage sample. The sample is found in the
	 * .percentage field of the message.
	 */
	BATTERY_PERCENTAGE_SAMPLE_RESPONSE = 0x1,

	/* Input message types */

	/* Request to retrieve the current battery percentage. The response is sent as a
	 * BATTERY_PERCENTAGE_SAMPLE_RESPONSE message.
	 */
	BATTERY_PERCENTAGE_SAMPLE_REQUEST,
};

struct battery_msg {
	enum battery_msg_type type;

	/** Contains the current charge of the battery in percentage. */
	double percentage;
};

#define MSG_TO_BATTERY_MSG(_msg)	(*(const struct battery_msg *)_msg)

#ifdef __cplusplus
}
#endif

#endif /* _BATTERY_H_ */
