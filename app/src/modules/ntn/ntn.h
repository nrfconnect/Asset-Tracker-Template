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

#define NTN_SIB32_MAX_LEN 512
#define NTN_SIB31_MAX_LEN 512
#define NTN_SIB31_MAX_FIELD_COUNT 32
#define GNSS_GPS_PI 3.1415926535898
/* Saturate value x to the range [MIN_VALUE..MAX_VALUE] */
#define SATURATE(MIN_VALUE, x, MAX_VALUE) \
	((x) < (MIN_VALUE) ? (MIN_VALUE) : ((x) > (MAX_VALUE) ? (MAX_VALUE) : (x)))

/* NTN module message types */
enum ntn_msg_type {
	/* NTN location request received */
	NTN_LOCATION_REQUEST,
	/* NTN location search completed */
	NTN_LOCATION_SEARCH_DONE,
	/* NTN trigger */
	NTN_TRIGGER,
	/* Network connectivity established */
	NTN_NETWORK_CONNECTED,
	/* Network connectivity lost */
	NTN_NETWORK_DISCONNECTED,
	/*  */
	NTN_NETWORK_CONNECTION_FAILED,
	/*  */
	NTN_NETWORK_CONNECTION_TIMEOUT,
	/* PDN connection resumed (context preserved) */
	NTN_PDN_RESUMED,
	/* RRC connected */
	NTN_RRC_CONNECTED,
	/* RRC idle */
	NTN_RRC_IDLE,
	/* Cell found / modem reports searching or registered */
	NTN_CELL_FOUND,
	/* Modem registered on NTN network (CEREG=1 or 5) */
	NTN_NETWORK_REGISTERED,
	/* RRC connected timeout */
	RRC_CONNECTED_TIMEOUT,
	/* GNSS search timeout */
	GNSS_TIMEOUT,
	/* Set SIB32 prediction data from shell or AT monitor */
	NTN_SET_SIB32,
	/* Set SIB31 prediction data from shell or AT monitor */
	NTN_SET_SIB31,
};

/* NTN module message */
struct ntn_msg {
	enum ntn_msg_type type;
	struct nrf_modem_gnss_pvt_data_frame pvt;
	char sib32_data[NTN_SIB32_MAX_LEN];
	char sib31_data[NTN_SIB31_MAX_LEN];
};

/* Declare the NTN message channel */
ZBUS_CHAN_DECLARE(NTN_CHAN);

#ifdef __cplusplus
}
#endif

#endif /* NTN_H */
