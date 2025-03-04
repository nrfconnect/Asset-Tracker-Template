/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

void check_network_event(enum network_msg_type expected_network_type);

void check_power_event(enum power_msg_type expected_power_type);

void check_no_events(uint32_t time_in_seconds);
