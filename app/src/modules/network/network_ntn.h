/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _NETWORK_NTN_H_
#define _NETWORK_NTN_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <modem/lte_lc.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Standard TLE line length (69 characters) plus null terminator. */
#define NETWORK_TLE_LINE_LEN	70

/** Macro to cast the incoming message buffer to a network_msg struct.
 * @param msg The message buffer to cast to a network_msg struct.
 */
#define MSG_TO_NETWORK_MSG(_msg)	(*(const struct network_msg *)_msg)

/* Channels provided by this module */
ZBUS_CHAN_DECLARE(
	NETWORK_CHAN
);

/** Network module message types. */
enum network_msg_type {
	/* Output message types */

	/* The device is disconnected from the network */
	NETWORK_DISCONNECTED = 0x1,

	/* The device is connected to the network and has an IP address */
	NETWORK_CONNECTED,

	/* The modem has detected a reset loop with too many attach requests within a short time.
	 * Refer to the AT command manual for more information on the details for your specific
	 * modem firmware version.
	 */
	NETWORK_MODEM_RESET_LOOP,

	/* The modem has detected an error with the SIM card. Confirm that it is installed
	 * correctly.
	 */
	NETWORK_UICC_FAILURE,

	/* The modem has completed a light search based on previous cell history for network to
	 * attach to without finding a suitable cell according to 3GPP selection rules.
	 * The modem will continue with a more thorough search unless it is explicitly stopped.
	 */
	NETWORK_LIGHT_SEARCH_DONE,

	/* The modem has completed a full search for a network to attach to without finding a
	 * suitable cell according to 3GPP selection rules.
	 * The modem will continue to search periodically for a suitable cell if it left in
	 * normal mode.
	 */
	NETWORK_SEARCH_DONE,

	/* A network attach request has been rejected by the network. */
	NETWORK_ATTACH_REJECTED,

	/* Updated PSM parameters have been received and are found as payload in the message.
	 * The parameters are located in the .psm_cfg field of the message.
	 */
	NETWORK_PSM_PARAMS,

	/* Updated eDRX parameters have been received and are found as payload in the message.
	 * The parameters are located in the .edrx_cfg field of the message.
	 */
	NETWORK_EDRX_PARAMS,

	/* Response message to a request for the current system mode. The current system mode is
	 * found in the .system_mode field of the message. In this context, "current system mode"
	 * refers to the system mode that the modem is currently configured to use and may include
	 * both LTE-M and NB-IoT. It does not reflect if the modem is currently connected to a
	 * network using a specific system mode.
	 */
	NETWORK_SYSTEM_MODE_RESPONSE,

	/* The modem requires a fresh location fix for NTN. The application should respond with a
	 * GNSS-only location request via the location module and respond with
	 * NETWORK_LOCATION_DATA on success or NETWORK_LOCATION_FAILED on failure.
	 */
	NETWORK_LOCATION_NEEDED,

	/* The modem has completed an NTN satellite search without finding a suitable cell. */
	NETWORK_NTN_NO_SUITABLE_CELL,

	/* Input message types */

	/* Request to connect to a terrestrial network (LTE-M / NB-IoT), which includes searching
	 * for a suitable network and attempting to attach to it if a usable cell is found.
	 */
	NETWORK_CONNECT_TN,

	/* Request to initiate NTN connectivity (LEO/GEO satellite search). This triggers
	 * the NTN search preparation flow including location acquisition and pass estimation.
	 */
	NETWORK_CONNECT_NTN,

	/* Request to disconnect from the network */
	NETWORK_DISCONNECT,

	/* Stop searching for a network. This will stop the modem's search for a suitable network
	 * and it will not resume before explicitly requested to do so by a NETWORK_CONNECT_TN
	 * or NETWORK_CONNECT_NTN message.
	 */
	NETWORK_SEARCH_STOP,

	/* Request to set the system mode to only use LTE-M access technology.
	 * Note that the network module must be disconnected and idle, ie. not actively searching
	 * for the system mode to be set.
	 */
	NETWORK_SYSTEM_MODE_SET_LTEM,

	/* Request to set the system mode to only use NB-IoT access technology.
	 * Note that the network module must be disconnected and idle, ie. not actively searching
	 * for the system mode to be set.
	 */
	NETWORK_SYSTEM_MODE_SET_NBIOT,

	/* Request to set the system mode use both LTE-M and NB-IoT access technologies
	 * when searching for a network.
	 * Note that the network module must be disconnected and idle, ie. not actively searching
	 * for the system mode to be set.
	 */
	NETWORK_SYSTEM_MODE_SET_LTEM_NBIOT,

	/* Request to retrieve the current system mode. The response is sent as a
	 * NETWORK_SYSTEM_MODE_RESPONSE message.
	 */
	NETWORK_SYSTEM_MODE_REQUEST,

	/* Location fix result from application mediation layer. Contains latitude, longitude,
	 * and altitude obtained from a GNSS-only fix. Payload in the .location field.
	 */
	NETWORK_LOCATION_DATA,

	/* Location fix failed. Sent by the application mediation layer when the location
	 * module could not obtain a GNSS fix.
	 */
	NETWORK_LOCATION_FAILED,

	/* New TLE (Two-Line Element) orbital data available for LEO satellite pass
	 * prediction. Payload in the .tle field.
	 */
	NETWORK_TLE_DATA,
};

/** Two-Line Element orbital data for LEO satellite pass prediction. */
struct network_tle {
	/** TLE line 1. */
	char line1[NETWORK_TLE_LINE_LEN];

	/** TLE line 2. */
	char line2[NETWORK_TLE_LINE_LEN];
};

/** Location data for NTN operations. Contains the fields required by ntn_location_set(). */
struct network_location {
	/** Geodetic latitude (deg) in WGS-84. */
	double latitude;

	/** Geodetic longitude (deg) in WGS-84. */
	double longitude;

	/** Altitude above WGS-84 ellipsoid in meters. */
	float altitude;
};

/** Network module message structure. */
struct network_msg {
	/** Message type. */
	enum network_msg_type type;

	union {
		/** Contains the currently configured system mode.
		 *  system_mode is set for NETWORK_SYSTEM_MODE_RESPONSE events
		 */
		enum lte_lc_system_mode system_mode;

		/** Contains the current PSM configuration.
		 *  psm_cfg is valid for NETWORK_PSM_PARAMS events.
		 */
		IF_ENABLED(CONFIG_LTE_LC_PSM_MODULE, (struct lte_lc_psm_cfg psm_cfg));

		/** Contains the current eDRX configuration.
		 *  edrx_cfg is valid for NETWORK_EDRX_PARAMS events.
		 */
		IF_ENABLED(CONFIG_LTE_LC_EDRX_MODULE, (struct lte_lc_edrx_cfg edrx_cfg));

		/** Location fix result from GNSS.
		 *  Valid for NETWORK_LOCATION_DATA events.
		 */
		struct network_location location;

		/** TLE orbital data.
		 *  Valid for NETWORK_TLE_DATA events.
		 */
		struct network_tle tle;
	};

	/** Timestamp when the sample was taken in milliseconds.
	 *  This is either:
	 * - Unix time in milliseconds if the system clock was synchronized at sampling time, or
	 * - Uptime in milliseconds if the system clock was not synchronized at sampling time.
	 */
	int64_t timestamp;
};

#ifdef __cplusplus
}
#endif

#endif /* _NETWORK_NTN_H_ */
