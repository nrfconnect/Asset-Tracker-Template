/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _POWER_H_
#define _POWER_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Channels provided by this module */
ZBUS_CHAN_DECLARE(
	POWER_CHAN
);

enum power_msg_type {
	/* Output message types */

	/* Response message to a request for a battery percentage sample. The sample is found in the
	 * .percentage field of the message.
	 */
	POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE = 0x1,

	/* Input message types */

	/* Request to retrieve the current battery percentage. The response is sent as a
	 * POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE message.
	 */
	POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST,
};

struct power_msg {
	enum power_msg_type type;

	/** Contains the current charge of the battery in percentage. */
	double percentage;

	/** True if the battery is charging, false otherwise. */
	bool charging;

	/** Voltage in volts. */
	double voltage;

	/** Uptime when the sample was taken in milliseconds.
	 *  Use date_time_uptime_to_unix_time_ms() to convert to unix time before sending to cloud.
	 *  Only valid for POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE events.
	 */
	int64_t uptime;
};

#define MSG_TO_POWER_MSG(_msg)	(*(const struct power_msg *)_msg)

#ifdef __cplusplus
}
#endif

#endif /* _POWER_H_ */
