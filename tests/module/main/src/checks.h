/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "location.h"
#include "network.h"
#include "power.h"
#include "fota.h"

void expect_location_event(enum location_msg_type expected_location_type);

void expect_network_event(enum network_msg_type expected_network_type);

void expect_power_event(enum power_msg_type expected_power_type);

void expect_fota_event(enum fota_msg_type expected_fota_type);

void expect_no_events(uint32_t timeout_sec);

void purge_location_events(void);

void purge_network_events(void);

void purge_power_events(void);

void purge_all_events(void);

/* Wait for a location event of the expected type.
 *
 * Returns the time in seconds it took to receive the event if it is received within the timeout.
 * Otherwise, it returns a negative value.
 */
int wait_for_location_event(enum location_msg_type expected_type, uint32_t timeout_sec);
