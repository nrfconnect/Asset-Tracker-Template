/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef REDEF_H_
#define REDEF_H_

#include <zephyr/device.h>

extern struct device mock_charger_device;

#undef DEVICE_DT_GET
#define DEVICE_DT_GET(node_id) &mock_charger_device

#endif /* REDEF_ */
