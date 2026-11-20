/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include <string.h>

#include <unity.h>
#include <zephyr/fff.h>
#include <zephyr/sys/iterable_sections.h>

#include <memfault/nrfconnect_port/fota_component.h>
#include <net/fota_download.h>
#include <nrf_cloud_bootloader_version.h>

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(int, fota_download_s0_active_get, bool *const);
FAKE_VALUE_FUNC(int, nrf_cloud_bootloader_version_string_get, char *, size_t);

#define SOFTWARE_TYPE_S0   "mcuboot-s0"
#define SOFTWARE_TYPE_S1   "mcuboot-s1"
#define BOOTLOADER_VERSION "5"

static bool s0_active;

static int s0_active_get_custom(bool *const active)
{
	*active = s0_active;

	return 0;
}

static int version_string_get_custom(char *buf, size_t len)
{
	strncpy(buf, BOOTLOADER_VERSION, len);

	return 0;
}

static const sMemfaultFotaComponent *component_get(const char *software_type)
{
	STRUCT_SECTION_FOREACH(MemfaultFotaComponent, component) {
		if (strcmp(component->software_type, software_type) == 0) {
			return component;
		}
	}

	return NULL;
}

void setUp(void)
{
	RESET_FAKE(fota_download_s0_active_get);
	RESET_FAKE(nrf_cloud_bootloader_version_string_get);
	FFF_RESET_HISTORY();

	s0_active = true;
	fota_download_s0_active_get_fake.custom_fake = s0_active_get_custom;
	nrf_cloud_bootloader_version_string_get_fake.custom_fake = version_string_get_custom;
}

void tearDown(void)
{
}

void test_components_are_registered_as_bootloader_updates(void)
{
	const sMemfaultFotaComponent *s0 = component_get(SOFTWARE_TYPE_S0);
	const sMemfaultFotaComponent *s1 = component_get(SOFTWARE_TYPE_S1);

	TEST_ASSERT_NOT_NULL(s0);
	TEST_ASSERT_NOT_NULL(s1);
	TEST_ASSERT_NOT_NULL(s0->version_get);
	TEST_ASSERT_NOT_NULL(s1->version_get);
	TEST_ASSERT_EQUAL(kMemfaultFotaComponentInstall_Bootloader, s0->install);
	TEST_ASSERT_EQUAL(kMemfaultFotaComponentInstall_Bootloader, s1->install);
}

void test_only_the_inactive_slot_is_updated_when_s0_is_active(void)
{
	char version[16] = {0};

	s0_active = true;

	TEST_ASSERT_EQUAL(-ENOTSUP,
			  component_get(SOFTWARE_TYPE_S0)->version_get(version, sizeof(version)));
	TEST_ASSERT_EQUAL(0,
			  component_get(SOFTWARE_TYPE_S1)->version_get(version, sizeof(version)));
	TEST_ASSERT_EQUAL_STRING(BOOTLOADER_VERSION, version);
}

void test_only_the_inactive_slot_is_updated_when_s1_is_active(void)
{
	char version[16] = {0};

	s0_active = false;

	TEST_ASSERT_EQUAL(-ENOTSUP,
			  component_get(SOFTWARE_TYPE_S1)->version_get(version, sizeof(version)));
	TEST_ASSERT_EQUAL(0,
			  component_get(SOFTWARE_TYPE_S0)->version_get(version, sizeof(version)));
	TEST_ASSERT_EQUAL_STRING(BOOTLOADER_VERSION, version);
}

void test_version_is_not_read_for_the_active_slot(void)
{
	char version[16] = {0};

	s0_active = true;

	(void)component_get(SOFTWARE_TYPE_S0)->version_get(version, sizeof(version));

	TEST_ASSERT_EQUAL(0, nrf_cloud_bootloader_version_string_get_fake.call_count);
}

void test_version_is_copied_into_the_given_buffer(void)
{
	char version[16] = {0};

	s0_active = true;

	TEST_ASSERT_EQUAL(0,
			  component_get(SOFTWARE_TYPE_S1)->version_get(version, sizeof(version)));

	TEST_ASSERT_EQUAL(1, nrf_cloud_bootloader_version_string_get_fake.call_count);
	TEST_ASSERT_EQUAL_STRING(BOOTLOADER_VERSION, version);
}

void test_version_that_does_not_fit_the_buffer_is_rejected(void)
{
	char version[1] = {0};

	s0_active = true;

	TEST_ASSERT_EQUAL(-ENOMEM,
			  component_get(SOFTWARE_TYPE_S1)->version_get(version, sizeof(version)));
}

void test_active_slot_error_is_passed_on(void)
{
	char version[16] = {0};

	fota_download_s0_active_get_fake.custom_fake = NULL;
	fota_download_s0_active_get_fake.return_val = -EIO;

	TEST_ASSERT_EQUAL(-EIO,
			  component_get(SOFTWARE_TYPE_S0)->version_get(version, sizeof(version)));
	TEST_ASSERT_EQUAL(-EIO,
			  component_get(SOFTWARE_TYPE_S1)->version_get(version, sizeof(version)));
	TEST_ASSERT_EQUAL(0, nrf_cloud_bootloader_version_string_get_fake.call_count);
}

void test_version_error_is_passed_on(void)
{
	char version[16] = {0};

	s0_active = true;
	nrf_cloud_bootloader_version_string_get_fake.custom_fake = NULL;
	nrf_cloud_bootloader_version_string_get_fake.return_val = -ENOENT;

	TEST_ASSERT_EQUAL(-ENOENT,
			  component_get(SOFTWARE_TYPE_S1)->version_get(version, sizeof(version)));
}

/* This is needed because AssetTracker Template does not use the standard Zephyr main(). */
extern int unity_main(void);

int main(void)
{
	/* use the runner from test_runner_generate() */
	(void)unity_main();

	return 0;
}
