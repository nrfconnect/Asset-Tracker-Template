/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef REDEF_H
#define REDEF_H

#include <zephyr/kernel.h>

#define SYS_REBOOT_COLD 1

void sys_reboot(int type);

/* Rename app's main to app_main */
#define main app_main

/* Declare app_main so we can use it in the test */
extern int app_main(void);

K_THREAD_DEFINE(app_main_id,
		16384,
		app_main, NULL, NULL, NULL, 0, 0, 0);

#endif
