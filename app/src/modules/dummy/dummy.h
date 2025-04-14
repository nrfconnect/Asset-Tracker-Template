/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _DUMMY_H_
#define _DUMMY_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Module's zbus channel */
ZBUS_CHAN_DECLARE(DUMMY_CHAN);

/* Module message types */
enum dummy_msg_type {
    /* Output message types */
    DUMMY_SAMPLE_RESPONSE = 0x1,

    /* Input message types */
    DUMMY_SAMPLE_REQUEST,
};

/* Module message structure */
struct dummy_msg {
    enum dummy_msg_type type;
    int32_t value;
};

#define MSG_TO_DUMMY_MSG(_msg) (*(const struct dummy_msg *)_msg)

#ifdef __cplusplus
}
#endif

#endif /* _DUMMY_H_ */
