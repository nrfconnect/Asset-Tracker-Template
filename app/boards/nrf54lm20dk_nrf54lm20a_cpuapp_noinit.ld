/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Place Memfault noinit symbols away from the main image when using
 * CONFIG_MEMFAULT_REBOOT_TRACKING_REGION / CONFIG_MEMFAULT_RAM_BACKED_COREDUMP_REGION
 * on nRF54L (see Memfault nRF Connect SDK examples).
 */

SECTION_PROLOGUE (memfault_noinit, 0x20020000 (NOLOAD),)
{
	KEEP(*(*.memfault_reboot_info));
	KEEP(*(*.memfault_coredump));
} GROUP_DATA_LINK_IN(RAMABLE_REGION, RAMABLE_REGION)
