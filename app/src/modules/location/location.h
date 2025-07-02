/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _LOCATION_H_
#define _LOCATION_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <modem/location.h>
#include <modem/lte_lc.h>
#include <nrf_modem_gnss.h>
#include <net/wifi_location_common.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Channels provided by this module */
ZBUS_CHAN_DECLARE(
	LOCATION_CHAN
);

enum location_msg_type {
	LOCATION_SEARCH_STARTED = 0x1,
	LOCATION_SEARCH_DONE,
	LOCATION_SEARCH_TRIGGER,
	LOCATION_CLOUD_REQUEST,
	LOCATION_AGNSS_REQUEST,
	LOCATION_GNSS_DATA,
	LOCATION_SEARCH_CANCEL,
};

/* Structure to pass location data through zbus */
struct location_msg {
	enum location_msg_type type;
	union {
		struct location_data_cloud cloud_request;
		struct nrf_modem_gnss_agnss_data_frame agnss_request;
		struct location_data gnss_data;
	};
};

#define MSG_TO_LOCATION_TYPE(_msg)	(((const struct location_msg *)_msg)->type)
#define MSG_TO_LOCATION_MSG_PTR(_msg)	(((const struct location_msg *)_msg))

#ifdef __cplusplus
}
#endif

#endif /* _LOCATION_H_ */
