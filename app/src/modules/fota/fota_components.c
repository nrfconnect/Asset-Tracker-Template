/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/* OTA components delivered through Memfault multi-component OTA, in addition to the application
 * itself (the primary component) and the modem firmware (registered by the Memfault SDK as
 * CONFIG_MEMFAULT_FOTA_MODEM_SOFTWARE_TYPE).
 *
 * To add a component: write a function that reports its running version, and register it with
 * MEMFAULT_FOTA_NON_PRIMARY_COMPONENT_DEFINE().
 */

#include <errno.h>
#include <stdio.h>

#include <memfault/nrfconnect_port/fota_non_primary_component.h>
#include <net/fota_download.h>
#include <nrf_cloud_bootloader_version.h>

#define BOOTLOADER_VERSION_PREFIX "mcuboot_"

/* The bootloader is updated through the inactive of two B1 slots, and each slot needs its own
 * image variant. Each variant is a separate Memfault software type, and only the one targeting
 * the inactive slot is queried. Upload to both software types, with the same component version:
 *   mcuboot-s0: signed_by_mcuboot_and_b0_mcuboot.bin
 *   mcuboot-s1: signed_by_mcuboot_and_b0_mcuboot_s1_variant.bin
 * The component version is "mcuboot_<mcuboot CONFIG_FW_INFO_FIRMWARE_VERSION>", e.g. "mcuboot_2".
 */
static int bootloader_version_get(bool target_s0, char *buf, size_t buf_len)
{
	int err;
	bool s0_active;
	char version[16];

	err = fota_download_s0_active_get(&s0_active);
	if (err) {
		return err;
	}

	/* The update goes to the inactive slot: s0 is targeted when s1 is running and vice versa. */
	if (s0_active == target_s0) {
		return -ENOTSUP;
	}

	err = nrf_cloud_bootloader_version_string_get(version, sizeof(version));
	if (err) {
		return err;
	}

	if (snprintf(buf, buf_len, BOOTLOADER_VERSION_PREFIX "%s", version) >= (int)buf_len) {
		return -ENOMEM;
	}

	return 0;
}

static int bootloader_s0_version_get(char *buf, size_t buf_len)
{
	return bootloader_version_get(true, buf, buf_len);
}

static int bootloader_s1_version_get(char *buf, size_t buf_len)
{
	return bootloader_version_get(false, buf, buf_len);
}

MEMFAULT_FOTA_NON_PRIMARY_COMPONENT_DEFINE(mcuboot_s0, "mcuboot-s0", bootloader_s0_version_get,
					   kMemfaultFotaNonPrimaryComponentInstall_Bootloader);
MEMFAULT_FOTA_NON_PRIMARY_COMPONENT_DEFINE(mcuboot_s1, "mcuboot-s1", bootloader_s1_version_get,
					   kMemfaultFotaNonPrimaryComponentInstall_Bootloader);
