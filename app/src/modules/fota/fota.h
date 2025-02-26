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
ZBUS_CHAN_DECLARE(FOTA_CHAN);

enum fota_msg_type {
	/* Output message types */

	/* Event notified when downloading the FOTA update failed. */
	FOTA_DOWNLOAD_FAILED = 0x1,

	/* Event notified when downloading the FOTA update timed out. */
	FOTA_DOWNLOAD_TIMED_OUT,

	/* Event notified when a FOTA update is being downloaded. */
	FOTA_DOWNLOADING_UPDATE,

	/* Event notified if there is no available update. */
	FOTA_NO_AVAILABLE_UPDATE,

	/* Event notified when a FOTA update has succeeded, reboot is needed to apply the image. */
	FOTA_SUCCESS_REBOOT_NEEDED,

	/* Event notified when the module needs the network to disconnect in order to apply
	 * an update. When disconnected from the network, send the event FOTA_IMAGE_APPLY.
	 * This is needed for Full Modem FOTA updates.
	 */
	FOTA_IMAGE_APPLY_NEEDED,

	/* Event notified when the FOTA download has been canceled. */
	FOTA_DOWNLOAD_CANCELED,

	/* Input message types */

	/* Request to poll cloud for any available firmware updates. */
	FOTA_POLL_REQUEST,

	/* Request to apply the downloaded firmware image. */
	FOTA_IMAGE_APPLY,

	/* Cancel the FOTA download. */
	FOTA_DOWNLOAD_CANCEL,
};

#define MSG_TO_FOTA_TYPE(_msg) (*(const enum fota_msg_type *)_msg)

#ifdef __cplusplus
}
#endif

#endif /* _FOTA_H_ */
