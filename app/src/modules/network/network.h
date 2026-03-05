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

#if defined(CONFIG_APP_NETWORK_NTN)
#include "network_ntn.h"
#else
#include "network_tn.h"
#endif

#ifdef __cplusplus
}
#endif

#endif /* _NETWORK_H_ */
