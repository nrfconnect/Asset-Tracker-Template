/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <nrf_fuel_gauge.h>
#include <errno.h>

#include "fuel_gauge_state.h"

LOG_MODULE_DECLARE(power, CONFIG_APP_POWER_LOG_LEVEL);

#define FUEL_GAUGE_MAGIC 0x4647534F /* "FGSO" - Fuel Gauge State OK */

struct fuel_gauge_state {
	uint32_t magic;     /* Magic number to identify valid state */
	uint32_t size;      /* Size of state data */
	uint8_t state[256]; /* Fuel gauge state buffer */
};

/* Place the fuel gauge state in the noinit RAM section.
 * This ensures the data persists across warm resets.
 */
static __noinit struct fuel_gauge_state fuel_gauge_noinit;

static bool fuel_gauge_state_is_valid(void)
{
	if (fuel_gauge_noinit.magic != FUEL_GAUGE_MAGIC) {
		LOG_DBG("No valid fuel gauge state found (magic: 0x%08x)",
			fuel_gauge_noinit.magic);
		return false;
	}

	if ((fuel_gauge_noinit.size == 0) ||
	    (fuel_gauge_noinit.size > sizeof(fuel_gauge_noinit.state))) {
		LOG_WRN("Invalid fuel gauge state size: %u", fuel_gauge_noinit.size);
		return false;
	}

	return true;
}

int fuel_gauge_state_save(void)
{
	int err;
	size_t state_size = nrf_fuel_gauge_state_size;

	if (state_size > sizeof(fuel_gauge_noinit.state)) {
		LOG_ERR("Fuel gauge state size too large: %zu", state_size);
		return -ENOMEM;
	}

	err = nrf_fuel_gauge_state_get(fuel_gauge_noinit.state, state_size);
	if (err) {
		LOG_ERR("nrf_fuel_gauge_state_get failed: %d", err);
		return err;
	}

	fuel_gauge_noinit.size = state_size;
	fuel_gauge_noinit.magic = FUEL_GAUGE_MAGIC;

	LOG_DBG("Saved fuel gauge state to no-init RAM (%zu bytes)", state_size);
	return 0;
}

const void *fuel_gauge_state_get(void)
{
	if (!fuel_gauge_state_is_valid()) {
		return NULL;
	}

	return fuel_gauge_noinit.state;
}

size_t fuel_gauge_state_size_get(void)
{
	if (!fuel_gauge_state_is_valid()) {
		return 0;
	}

	return fuel_gauge_noinit.size;
}
