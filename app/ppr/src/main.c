/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
	while (true) {
		LOG_INF("hello world from %s", CONFIG_BOARD_TARGET);
		k_sleep(K_SECONDS(CONFIG_PPR_HELLO_INTERVAL_SECONDS));
	}

	return 0;
}
