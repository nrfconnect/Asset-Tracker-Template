/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _FOTA_H_
#define _FOTA_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Channels provided by this module */
ZBUS_CHAN_DECLARE(fota_chan);

enum fota_msg_type {
	/* Output message types */

	/* FOTA module is ready to use. */
	FOTA_MODULE_READY = 0x1,

	/* A FOTA download has started. */
	FOTA_STARTING,

	/* The module needs the network to be disconnected before it can
	 * continue. The application is expected to disconnect the network and
	 * reply with FOTA_NETWORK_DISCONNECTED when it is done.
	 */
	FOTA_NETWORK_DISCONNECT_NEEDED,

	/* The FOTA sequence completed successfully and the device is
	 * ready to reboot in order to apply the image.
	 */
	FOTA_SUCCESS,

	/* The FOTA sequence was aborted (download failed, timed out,
	 * canceled, rejected, or no update was available).
	 */
	FOTA_ABORTED,

	/* Request to poll cloud for any available firmware updates. */
	FOTA_POLL_REQUEST,

	/* Cancel the FOTA download. */
	FOTA_DOWNLOAD_CANCEL,

	/* Reply to FOTA_NETWORK_DISCONNECT_NEEDED indicating that the
	 * network has been disconnected and the FOTA module may continue.
	 */
	FOTA_NETWORK_DISCONNECTED,
};

struct fota_msg {
	enum fota_msg_type type;
};

#ifdef __cplusplus
}
#endif

#endif /* _FOTA_H_ */
