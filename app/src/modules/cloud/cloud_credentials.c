/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <net/nrf_cloud.h>
#include <modem/modem_key_mgmt.h>

#include "cloud_credentials.h"

LOG_MODULE_DECLARE(cloud, CONFIG_APP_CLOUD_LOG_LEVEL);

static int jwt_private_key_present_get(const struct nrf_cloud_credentials_status *const status,
				       uint32_t jwt_sec_tag, bool *const present)
{
	int err;

	if (jwt_sec_tag == status->sec_tag) {
		*present = status->prv_key;

		return 0;
	}

	err = modem_key_mgmt_exists(jwt_sec_tag, MODEM_KEY_MGMT_CRED_TYPE_PRIVATE_CERT, present);
	if (err) {
		LOG_ERR("modem_key_mgmt_exists, sec tag %u, error: %d", jwt_sec_tag, err);

		return err;
	}

	return 0;
}

int cloud_credentials_present_get(bool *const present)
{
	int err;
	bool private_key_present;
	struct nrf_cloud_credentials_status status;
	const uint32_t jwt_sec_tag = (uint32_t)nrf_cloud_sec_tag_coap_jwt_get();

	err = nrf_cloud_credentials_check(&status);
	if (err) {
		LOG_ERR("nrf_cloud_credentials_check, error: %d", err);

		return err;
	}

	err = jwt_private_key_present_get(&status, jwt_sec_tag, &private_key_present);
	if (err) {
		return err;
	}

	if (!status.ca) {
		LOG_WRN("No CA certificate in sec tag %u", status.sec_tag);
	}

	if (!private_key_present) {
		LOG_WRN("No private key in sec tag %u", jwt_sec_tag);
	}

	*present = status.ca && private_key_present;

	return 0;
}
