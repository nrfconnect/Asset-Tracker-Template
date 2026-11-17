/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _CLOUD_CREDENTIALS_H_
#define _CLOUD_CREDENTIALS_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check if the credentials needed to connect to nRF Cloud CoAP are stored on the device.
 *
 * The CA certificate is looked up in the nRF Cloud CoAP sec tag, and the private key used to
 * sign JWTs in the CoAP JWT sec tag. The two sec tags can differ, for example when
 * CONFIG_NRF_CLOUD_COAP_SEC_TAG points to a developer sec tag. A warning is logged for each
 * credential that is missing.
 *
 * @param[out] present Set to true if both the CA certificate and the private key are present,
 *                     false otherwise. Only valid when the function returns 0.
 *
 * @retval 0 The credentials were checked and @p present is valid.
 * @retval -EIO The nRF Cloud credentials check failed.
 * @return Other negative error codes from modem_key_mgmt_exists() if checking the private key
 *         in a separate JWT sec tag failed.
 */
int cloud_credentials_present_get(bool *const present);

#ifdef __cplusplus
}
#endif

#endif /* _CLOUD_CREDENTIALS_H_ */
