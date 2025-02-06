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
	FOTA_POLL = 0x1,
};

#define MSG_TO_FOTA_TYPE(_msg)	(*(const enum fota_msg_type *)_msg)

#ifdef __cplusplus
}
#endif

#endif /* _FOTA_H_ */
