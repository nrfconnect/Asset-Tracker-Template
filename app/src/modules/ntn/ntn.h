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
	/* Events that trigger state transitions */
	NTN_LOCATION_SEARCH_DONE, /* Location search completed - triggers transition to NTN mode */
	NTN_NETWORK_CONNECTED,    /* Network connectivity established */
	NTN_SET_IDLE,            /* Set idle state */
	GNSS_TRIGGER,          /* GNSS timer expired - get new fix */
	NTN_TRIGGER,           /* LTE timer expired - connect to network */
	NTN_SHELL_SET_TIME,  /* Set new time of pass from shell */
	KEEPALIVE_TIMER,     /* Keepalive timer */
	NTN_RRC_CONNECTED,   /* RRC connected */
	RESCHEDULE_NTN_TRIGGER, /* RESCHEDULE_NTN_TRIGGER */
	SET_IDLE_TIMER, /* SET_IDLE_TIMER */
};

/* NTN module message */
struct ntn_msg {
	enum ntn_msg_type type;
	char time_of_pass[32];  /* For NTN_SHELL_SET_TIME */
	struct nrf_modem_gnss_pvt_data_frame pvt;
};

/* Declare the NTN message channel */
ZBUS_CHAN_DECLARE(NTN_CHAN);

#ifdef __cplusplus
}
#endif

#endif /* NTN_H */
