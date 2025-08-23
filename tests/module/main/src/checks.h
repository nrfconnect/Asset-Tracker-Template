/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "location.h"
#include "network.h"
#include "power.h"
#include "fota.h"
#include "storage.h"
#include "cloud.h"

/* Private functions that return the actual received values or negative error codes */
int priv_expect_location_event(void);
int priv_expect_network_event(void);
int priv_expect_power_event(void);
int priv_expect_fota_event(void);
int priv_expect_storage_event(void);
int priv_expect_cloud_event(void);

/* Macros that provide line number information for test failures */
#define expect_location_event(expected) \
	TEST_ASSERT_EQUAL(expected, priv_expect_location_event())

#define expect_network_event(expected) \
	TEST_ASSERT_EQUAL(expected, priv_expect_network_event())

#define expect_power_event(expected) \
	TEST_ASSERT_EQUAL(expected, priv_expect_power_event())

#define expect_fota_event(expected) \
	TEST_ASSERT_EQUAL(expected, priv_expect_fota_event())

#define expect_storage_event(expected) \
	TEST_ASSERT_EQUAL(expected, priv_expect_storage_event())

#define expect_cloud_event(expected) \
	TEST_ASSERT_EQUAL(expected, priv_expect_cloud_event())

void expect_no_events(uint32_t timeout_sec);

void purge_location_events(void);

void purge_network_events(void);

void purge_power_events(void);

void purge_storage_events(void);

void purge_cloud_events(void);

void purge_all_events(void);

/* Wait for a location event of the expected type.
 *
 * Returns the time in seconds it took to receive the event if it is received within the timeout.
 * Otherwise, it returns a negative value.
 */
int wait_for_location_event(enum location_msg_type expected_type, uint32_t timeout_sec);
