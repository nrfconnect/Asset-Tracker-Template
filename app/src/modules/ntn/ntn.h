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

#define NTN_TLE_NAME_MAX_LEN 30
#define NTN_TLE_LINE_MAX_LEN 80
#define NTN_SIB32_MAX_LEN 512

/* NTN module message types */
enum ntn_msg_type {
	/* Events that trigger state transitions */
	NETWORK_CONNECTED,    /* Network connectivity established */
	NETWORK_DISCONNECTED, /* Network disconnected */
	NETWORK_CONNECTION_FAILED, /*  */
	NETWORK_CONNECTION_TIMEOUT, /*  */
	NTN_PDN_RESUMED, /* PDN connection resumed (context preserved) */
	NTN_RRC_CONNECTED, /* RRC connected */
	NTN_RRC_IDLE, /* RRC idle */
	NTN_CELL_FOUND, /* Cell found / modem reports searching or registered */
	NTN_NETWORK_REGISTERED, /* Modem registered on NTN network (CEREG=1 or 5) */
	LOCATION_SEARCH_DONE, /* Location search completed - triggers transition to NTN mode */
	LOCATION_REQUEST, /*  */
	GNSS_TIMEOUT, /* */
	NTN_TRIGGER,           /* LTE timer expired - connect to network */
	NTN_SHELL_SET_TIME,  /* Set new time of pass from shell */
	NTN_SHELL_SET_DATETIME, /* Set date time from shell */
	NTN_SHELL_SET_GNSS_LOCATION, /* Set GNSS location from shell */
	NTN_SHELL_SET_TLE, /* Set TLE from shell */
	NTN_SHELL_SET_PEAK_OFFSET, /* Set NTN activation offset from shell */
	NTN_SET_SIB32, /* Set SIB32 prediction data from shell or AT monitor */
	NTN_CLEAR_SIB32, /* Clear cached SIB32 prediction data */
	KEEPALIVE_TIMER,     /* Keepalive timer */
	SGP4_TRIGGER, /* */
	GNSS_TRIGGER, /* */
	IDLE_TRIGGER, /* Force IDLE state from shell */
};

/* NTN module message */
struct ntn_msg {
	enum ntn_msg_type type;
	float sgp4_min_elevation_deg;
	char time_of_pass[32];
	char datetime[32];
	char tle_name[NTN_TLE_NAME_MAX_LEN];
	char tle_line1[NTN_TLE_LINE_MAX_LEN];
	char tle_line2[NTN_TLE_LINE_MAX_LEN];
	int32_t peak_offset_seconds;
	char sib32_data[NTN_SIB32_MAX_LEN];
	struct nrf_modem_gnss_pvt_data_frame pvt;
};

/* Declare the NTN message channel */
ZBUS_CHAN_DECLARE(NTN_CHAN);

#ifdef __cplusplus
}
#endif

#endif /* NTN_H */
