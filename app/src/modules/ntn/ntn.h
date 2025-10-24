/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef NTN_H
#define NTN_H

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <nrf_modem_gnss.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NTN module message types */
enum ntn_msg_type {
	/* Location search completed and GNSS location data is availabl e*/
	NTN_LOCATION_SEARCH_DONE,

	/* NTN mode timeout occurred */
	NTN_TIMEOUT,

	/* PDN activated */
	PDN_ACTIVATED,

	/* PDN deactivated */
	PDN_DEACTIVATED,

	/* Registered to network */
	NETWORK_REGISTERED,

	/* Network deregistered */
	NETWORK_DEREGISTERED,

	/* Network connectivity established */
	NETWORK_CONNECTED,

	/* Network connectivity lost */
	NETWORK_DISCONNECTED,

	/* Network no suitable cell found */
	NETWORK_NO_SUITABLE_CELL,

	/* GNSS search failed */
	GNSS_SEARCH_FAILED,
};

/* NTN module message */
struct ntn_msg {
	enum ntn_msg_type type;
	struct nrf_modem_gnss_pvt_data_frame pvt;
};

/* Declare the NTN message channel */
ZBUS_CHAN_DECLARE(NTN_CHAN);

#ifdef __cplusplus
}
#endif

#endif /* NTN_H */
