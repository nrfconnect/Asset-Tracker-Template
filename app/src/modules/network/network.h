/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _NETWORK_H_
#define _NETWORK_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <modem/lte_lc.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Channels provided by this module */
ZBUS_CHAN_DECLARE(
	network_chan
);

enum network_msg_type {
	/* Output message types */

	/* The device is disconnected from the network */
	NETWORK_DISCONNECTED = 0x1,

	/* The device is connected over a terrestrial network and has an IP address */
	NETWORK_CONNECTED_TN,

	/* The device is connected over an NTN network and has an IP address */
	NETWORK_CONNECTED_NTN,

	/* Terrestrial light search did not find a suitable cell. A cell could still be found
	 * if full search is continued.
	 */
	NETWORK_TN_LIGHT_SEARCH_DONE,

	/* Terrestrial network search timed out or failed without a usable cell. */
	NETWORK_TN_SEARCH_FAILED,

	/* Non-terrestrial light search did not find a suitable cell. A cell could still be found
	 * if a full search is continued.
	 */
	NETWORK_NTN_LIGHT_SEARCH_DONE,

	/* NTN network search timed out or failed without a usable cell */
	NETWORK_NTN_SEARCH_FAILED,

	/* A GNSS location fix is required before NTN cell search can proceed.
	 * The Main module should trigger a GNSS-only location search and respond with
	 * NETWORK_GNSS_LOCATION or NETWORK_GNSS_LOCATION_FAILED.
	 */
	NETWORK_GNSS_LOCATION_REQ,

	/* The modem has detected an error with the SIM card. Confirm that it is installed
	 * correctly.
	 */
	NETWORK_UICC_FAILURE,

	/* Updated PSM parameters have been received and are found as payload in the message.
	 * The parameters are located in the .psm_cfg field of the message.
	 */
	NETWORK_PSM_PARAMS,

	/* Updated eDRX parameters have been received and are found as payload in the message.
	 * The parameters are located in the .edrx_cfg field of the message.
	 */
	NETWORK_EDRX_PARAMS,

	/* Input message types */

	/* Request to connect over a terrestrial network (LTE-M + NB-IoT).
	 * Includes searching for a suitable cell and attaching if found.
	 */
	NETWORK_CONNECT_TN,

	/* Request to connect over an NTN network (NB-IoT). Requires a valid location
	 * (cached or provided via NETWORK_GNSS_LOCATION) before cell search.
	 */
	NETWORK_CONNECT_NTN,

	/* Request to disconnect from the network */
	NETWORK_DISCONNECT,

	/* Stop searching for a network. This will stop the modem's search for a suitable
	 * network and it will not resume before explicitly requested to do so by a
	 * NETWORK_CONNECT_TN or NETWORK_CONNECT_NTN message.
	 */
	NETWORK_SEARCH_STOP,

	/* Provide a GNSS location fix for NTN search. Latitude, longitude, and altitude
	 * are found in the .location.lat, .location.lon, and .location.alt fields of the message.
	 */
	NETWORK_GNSS_LOCATION,

	/* GNSS location acquisition failed or timed out. The network module will abort
	 * the current NTN search attempt.
	 */
	NETWORK_GNSS_LOCATION_FAILED,
};

struct network_msg {
	enum network_msg_type type;
	union {
		/** GNSS location fix. Valid for NETWORK_GNSS_LOCATION events.
		 *  Latitude, longitude and altitude are used together, so they must share
		 *  one struct rather than alias each other as separate union members.
		 */
		struct {
			/** Latitude in degrees. */
			double lat;

			/** Longitude in degrees. */
			double lon;

			/** Altitude in meters. */
			float alt;
		} location;

		/** Request a fresh GNSS fix before the NTN cell search even when a still-valid
		 *  cached fix is available. Valid for NETWORK_CONNECT_NTN. Used when the fix
		 *  should double as a location sample: GNSS cannot run while the modem is in NTN
		 *  system mode, so the attach sequence is the only opportunity to acquire one.
		 */
		bool fresh_location;

		/** Contains the current PSM configuration.
		 *  psm_cfg is valid for NETWORK_PSM_PARAMS events.
		 */
		IF_ENABLED(CONFIG_LTE_LC_PSM_MODULE, (struct lte_lc_psm_cfg psm_cfg));

		/** Contains the current eDRX configuration.
		 *  edrx_cfg is valid for NETWORK_EDRX_PARAMS events.
		 */
		IF_ENABLED(CONFIG_LTE_LC_EDRX_MODULE, (struct lte_lc_edrx_cfg edrx_cfg));
	};
};

/**
 * @brief Check whether the modem is currently in NTN (NB-IoT) system mode.
 *
 * GNSS cannot run while the modem is in NTN system mode, so callers use this to avoid
 * operations that depend on GNSS (e.g. fetching A-GNSS assistance) while on NTN.
 *
 * @return true if the modem is in NTN system mode, false otherwise or on query error.
 */
bool network_in_ntn_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* _NETWORK_H_ */
