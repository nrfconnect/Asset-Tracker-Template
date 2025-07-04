/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _ENVIRONMENTAL_H_
#define _ENVIRONMENTAL_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif


/* Channels provided by this module */
ZBUS_CHAN_DECLARE(
	ENVIRONMENTAL_CHAN
);

enum environmental_msg_type {
	/* Output message types */

	/* Response message to a request for current environmental sensor values.
	 * The sampled values are found in the respective fields of the message structure.
	 */
	ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE = 0x1,

	/* Input message types */

	/* Request to sample the current environmental sensor values.
	 * The response is sent as a ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE message.
	 */
	ENVIRONMENTAL_SENSOR_SAMPLE_REQUEST,
};

struct environmental_msg {
	enum environmental_msg_type type;

	/** Contains the current temperature in celsius. */
	double temperature;

	/** Contains the current humidity in percentage. */
	double humidity;

	/** Contains the current pressure in Pa. */
	double pressure;

	/** Timestamp of the sample in milliseconds since epoch. */
	int64_t timestamp;
};

#define MSG_TO_ENVIRONMENTAL_MSG(_msg)	(*(const struct environmental_msg *)_msg)


#ifdef __cplusplus
}
#endif

#endif /* _ENVIRONMENTAL_H_ */
