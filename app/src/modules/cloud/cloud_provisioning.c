/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <net/nrf_provisioning.h>

#include "cloud_provisioning.h"
#include "app_common.h"
#include "network.h"

LOG_MODULE_DECLARE(cloud, CONFIG_APP_CLOUD_LOG_LEVEL);

/* Private cloud channel messages for provisioning events */
enum priv_cloud_msg {
	CLOUD_CONNECTION_FAILED,
	CLOUD_CONNECTION_SUCCESS,
	CLOUD_NOT_AUTHENTICATED,
	CLOUD_PROVISIONING_FINISHED,
	CLOUD_PROVISIONING_FAILED,
	CLOUD_BACKOFF_EXPIRED,
	CLOUD_SEND_REQUEST_FAILED,
};

/* Declare the private cloud channel defined in cloud.c */
ZBUS_CHAN_DECLARE(PRIV_CLOUD_CHAN);

static void nrf_provisioning_callback(const struct nrf_provisioning_callback_data *event)
{
	int err;
	enum priv_cloud_msg msg = CLOUD_PROVISIONING_FINISHED;
	enum network_msg_type nw_msg = NETWORK_DISCONNECT;

	switch (event->type) {
	case NRF_PROVISIONING_EVENT_NEED_LTE_DEACTIVATED:
		LOG_WRN("nRF Provisioning requires device to deactivate LTE");

		nw_msg = NETWORK_DISCONNECT;

		err = zbus_chan_pub(&NETWORK_CHAN, &nw_msg, K_SECONDS(1));
		if (err) {
			LOG_ERR("zbus_chan_pub, error: %d", err);
			SEND_FATAL_ERROR();
		}

		return;
	case NRF_PROVISIONING_EVENT_NEED_LTE_ACTIVATED:
		LOG_WRN("nRF Provisioning requires device to activate LTE");

		nw_msg = NETWORK_CONNECT;

		err = zbus_chan_pub(&NETWORK_CHAN, &nw_msg, K_SECONDS(1));
		if (err) {
			LOG_ERR("zbus_chan_pub, error: %d", err);
			SEND_FATAL_ERROR();
		}

		return;
	case NRF_PROVISIONING_EVENT_DONE:
		LOG_DBG("Provisioning finished");

		msg = CLOUD_PROVISIONING_FINISHED;

		k_sleep(K_SECONDS(10));

		break;
	case NRF_PROVISIONING_EVENT_NO_COMMANDS:
		LOG_WRN("No commands from the nRF Provisioning Service to process");
		LOG_WRN("Treating as provisioning finished");

		msg = CLOUD_PROVISIONING_FINISHED;

		/* Workaround: Wait some seconds before sending finished message.
		 * This is needed to be able to connect to getting authorized when connecting
		 * to nRF Cloud CoAP after provisioning. To be investigated further.
		 */
		k_sleep(K_SECONDS(10));

		break;
	case NRF_PROVISIONING_EVENT_FAILED_TOO_MANY_COMMANDS:
		LOG_ERR("Provisioning failed, too many commands for the device to handle");

		/* Provisioning failed due to receiving too many commands.
		 * Treat this as 'provisioning finished' to allow reconnection to nRF Cloud CoAP.
		 * The process will need to be restarted via the device shadow
		 * with an acceptable number of commands in the provisioning service list.
		 */
		msg = CLOUD_PROVISIONING_FINISHED;

		/* Workaround: Wait some seconds before sending finished message.
		 * This is needed to be able to connect to getting authorized when connecting
		 * to nRF Cloud CoAP after provisioning. To be investigated further.
		 */
		k_sleep(K_SECONDS(10));

		break;
	case NRF_PROVISIONING_EVENT_FAILED:
		LOG_ERR("Provisioning failed");

		msg = CLOUD_PROVISIONING_FAILED;

		break;
	case NRF_PROVISIONING_EVENT_FAILED_NO_VALID_DATETIME:
		LOG_ERR("Provisioning failed, no valid datetime reference");

		msg = CLOUD_PROVISIONING_FAILED;
		break;
	case NRF_PROVISIONING_EVENT_FAILED_DEVICE_NOT_CLAIMED:
		LOG_WRN("Provisioning failed, device not claimed");
		LOG_WRN("Claim the device using the device's attestation token on nrfcloud.com");
		LOG_WRN("\r\n\n%.*s.%.*s\r\n", event->token->attest_sz, event->token->attest,
					       event->token->cose_sz, event->token->cose);

		msg = CLOUD_PROVISIONING_FAILED;


		break;
	case NRF_PROVISIONING_EVENT_FAILED_WRONG_ROOT_CA:
		LOG_ERR("Provisioning failed, wrong CA certificate");

		SEND_FATAL_ERROR();

		return;
	case NRF_PROVISIONING_EVENT_FATAL_ERROR:
		LOG_ERR("Provisioning error");

		SEND_FATAL_ERROR();
		return;
	default:
		/* Don't care */
		return;
	}

	err = zbus_chan_pub(&PRIV_CLOUD_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

int cloud_provisioning_init(void)
{
	int err;

	err = nrf_provisioning_init(nrf_provisioning_callback);
	if (err) {
		LOG_ERR("nrf_provisioning_init, error: %d", err);
		return err;
	}

	return 0;
}

int cloud_provisioning_trigger(void)
{
	int err;

	err = nrf_provisioning_trigger_manually();
	if (err) {
		LOG_ERR("nrf_provisioning_trigger_manually, error: %d", err);
		return err;
	}

	return 0;
}
