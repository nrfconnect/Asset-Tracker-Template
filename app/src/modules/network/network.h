/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _NETWORK_H_
#define _NETWORK_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif


/* Channels provided by this module */
ZBUS_CHAN_DECLARE(
	NETWORK_CHAN
);

enum network_msg_type {
	/* Output message types */

	/* The device is disconnected from the network */
	NETWORK_DISCONNECTED = 0x1,

	/* The device is connected to the network and has an IP address */
	NETWORK_CONNECTED,

	/* The modem has detected a reset loop with too many attach requests within a short time.
	 * Refer to the AT command manual for more information on the details for your specific
	 * modem firmware version.
	 */
	NETWORK_MODEM_RESET_LOOP,

	/* The modem has detected an error with the SIM card. Confirm that it is installed
	 * correctly.
	 */
	NETWORK_UICC_FAILURE,

	/* The modem has completed a light search based on previous cell hostory for network to
	 * attach to without finding a suitable cell according to 3GPP selection rules.
	 * The modem will continue with a more thorough search unless it is explicitly stopped.
	 */
	NETWORK_LIGHT_SERACH_DONE,

	/* A network attach request has been rejected by the network. */
	NETWORK_ATTACH_REJECTED,

	/* Updated PSM parameters have been received and are found as payload in the message.
	 * The parameters are located in the .psm_cfg field of the message.
	 */
	NETWORK_PSM_PARAMS,

	/* Updated eDRX parameters have been received and are found as payload in the message.
	 * The parameters are located in the .edrx_cfg field of the message.
	 */
	NETWORK_EDRX_PARAMS,

	/* Response message to a request for the current system mode. The current system mode is
	 * found in the .system_mode field of the message. In this context, "current system mode"
	 * refers to the system mode that the modem is currently configured to use and may include
	 * both LTE-M and NB-IoT. It does not reflect if the modem is currently connected to a
	 * network using a specific system mode.
	 */
	NETWORK_SYSTEM_MODE_RESPONSE,

	/* Response message to a request for a network quality sample. The sample is found in the
	 * .conn_eval_params field of the message.
	 */
	NETWORK_QUALITY_SAMPLE_RESPONSE,

	/* Input message types */

	/* Request to connect to the network, which includes searching for a suitable network
	 * and attempting to attach to it if a usable cell is found.
	 */
	NETWORK_CONNECT,

	/* Request to disconnect from the network */
	NETWORK_DISCONNECT,

	/* Stop searching for a network. This will stop the modem's search for a suitable network
	 * and it will not resume before explicitly requested to do so by a NETWORK_CONNECT message.
	 */
	NETWORK_SEARCH_STOP,

	/* Request to sample the current network connection quality. The result is sent as a
	 * NETWORK_QUALITY_SAMPLE_RESPONSE message if possible to obtain.
	 */
	NETWORK_QUALITY_SAMPLE_REQUEST,

	/* Request to set the system mode to only use LTE-M access technology.
	 * Note that the network module must be disconnected and idle, ie. not actively searching
	 * for the system mode to be set.
	 */
	NETWORK_SYSTEM_MODE_SET_LTEM,

	/* Request to set the system mode to only use NB-IoT access technology.
	 * Note that the network module must be disconnected and idle, ie. not actively searching
	 * for the system mode to be set.
	 */
	NETWORK_SYSTEM_MODE_SET_NBIOT,

	/* Request to set the system mode use both LTE-M and NB-IoT access technologies
	 * when searching for a network.
	 * Note that the network module must be disconnected and idle, ie. not actively searching
	 * for the system mode to be set.
	 */
	NETWORK_SYSTEM_MODE_SET_LTEM_NBIOT,

	/* Request to retrieve the current system mode. The response is sent as a
	 * NETWORK_SYSTEM_MODE_RESPONSE message.
	 */
	NETWORK_SYSTEM_MODE_REQUEST,
};

struct network_msg {
	enum network_msg_type type;
	union {
		/** Contains the currently configured system mode.
		 *  system_mode is set for NETWORK_SYSTEM_MODE_RESPONSE events
		 */
		enum lte_lc_system_mode system_mode;

		/** Contains the current PSM configuration.
		 *  psm_cfg is valid for NETWORK_PSM_PARAMS events.
		 */
		IF_ENABLED(CONFIG_LTE_LC_PSM_MODULE, (struct lte_lc_psm_cfg psm_cfg));

		/** Contains the current eDRX configuration.
		 *  edrx_cfg is valid for NETWORK_EDRX_PARAMS events.
		 */
		IF_ENABLED(CONFIG_LTE_LC_EDRX_MODULE, (struct lte_lc_edrx_cfg edrx_cfg));

		/** Contains the current connection evaluation information.
		 *  conn_eval_params is valid for NETWORK_QUALITY_SAMPLE_REQUEST events.
		 */
		IF_ENABLED(CONFIG_LTE_LC_CONN_EVAL_MODULE,
			   (struct lte_lc_conn_eval_params conn_eval_params));
	};
};

#define MSG_TO_NETWORK_MSG(_msg)	(*(const struct network_msg *)_msg)


#ifdef __cplusplus
}
#endif

#endif /* _NETWORK_H_ */
