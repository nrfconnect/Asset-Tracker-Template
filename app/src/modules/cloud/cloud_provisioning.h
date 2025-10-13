/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _CLOUD_PROVISIONING_H_
#define _CLOUD_PROVISIONING_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize nRF Cloud provisioning service.
 *
 * @return 0 on success, negative error code otherwise.
 */
int cloud_provisioning_init(void);

/**
 * @brief Trigger provisioning manually.
 *
 * @return 0 on success, negative error code otherwise.
 */
int cloud_provisioning_trigger(void);

#ifdef __cplusplus
}
#endif

#endif /* _CLOUD_PROVISIONING_H_ */
