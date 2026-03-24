/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdint.h>
#include <zephyr/logging/log.h>

/* Register log module */
LOG_MODULE_REGISTER(main, 4);

int main(void)
{
	LOG_DBG("Starting application");
}
