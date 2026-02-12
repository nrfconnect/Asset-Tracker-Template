/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _FUEL_GAUGE_STATE_H_
#define _FUEL_GAUGE_STATE_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Save the current fuel gauge state to no-init RAM.
 *
 * Retrieves the current state from the nRF Fuel Gauge library and stores it
 * in a no-init RAM section. This allows the state to persist across warm resets.
 *
 * @return 0 on success, negative error code on failure
 */
int fuel_gauge_state_save(void);

/**
 * @brief Get pointer to saved fuel gauge state.
 *
 * Returns a pointer to the saved fuel gauge state buffer if valid state exists.
 * The state is considered valid if the magic number matches and the size is within bounds.
 *
 * @return Pointer to state buffer on success, NULL if no valid state exists
 */
const void *fuel_gauge_state_get(void);

/**
 * @brief Get size of saved fuel gauge state.
 *
 * @return Size of saved state in bytes, or 0 if no valid state exists
 */
size_t fuel_gauge_state_size_get(void);

#ifdef __cplusplus
}
#endif

#endif /* _FUEL_GAUGE_STATE_H_ */
