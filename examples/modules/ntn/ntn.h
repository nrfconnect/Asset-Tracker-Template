/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _NTN_H_
#define _NTN_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Channels provided by this module */
ZBUS_CHAN_DECLARE(
	ntn_chan
);

enum ntn_msg_type {
	/* Output message types */

	/* The terrestrial network has been unavailable for too long. The module has suspended the
	 * network module and taken over the modem to use the non-terrestrial network (NTN).
	 */
	NTN_FALLBACK_STARTED = 0x1,

	/* The module has handed the modem back to the network module, which searches for a
	 * terrestrial network again.
	 */
	NTN_FALLBACK_STOPPED,

	/* A GNSS location was sent to nRF Cloud over NTN. */
	NTN_UPLINK_SENT,

	/* Sending a GNSS location to nRF Cloud over NTN failed or timed out. */
	NTN_UPLINK_FAILED,

	/* Input message types */

	/* Request to fall back to NTN without waiting for the terrestrial network timeout. */
	NTN_FALLBACK_START_REQUEST,

	/* Request to stop using NTN and search for a terrestrial network. */
	NTN_FALLBACK_STOP_REQUEST,

	/* Request to send the last known location over NTN, ignoring the minimum uplink interval.
	 * A GNSS search is triggered first if no valid location is available.
	 * Only handled while the module is using NTN.
	 */
	NTN_UPLINK_REQUEST,

	/* Set the device location used for NTN, for example when GNSS is not available indoors.
	 * The location is found in the .location field of the message.
	 */
	NTN_LOCATION_SET,
};

struct ntn_location {
	/* Latitude in degrees */
	double latitude;

	/* Longitude in degrees */
	double longitude;

	/* Altitude in meters */
	float altitude;

	/* Accuracy in meters */
	float accuracy;
};

struct ntn_msg {
	enum ntn_msg_type type;
	union {
		/* Location, valid for NTN_LOCATION_SET */
		struct ntn_location location;
	};
};

#ifdef __cplusplus
}
#endif

#endif /* _NTN_H_ */
