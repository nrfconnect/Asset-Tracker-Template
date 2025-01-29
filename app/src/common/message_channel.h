/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _MESSAGE_CHANNEL_H_
#define _MESSAGE_CHANNEL_H_

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/zbus/zbus.h>
#include <modem/lte_lc.h>
#if defined(CONFIG_MEMFAULT)
#include <memfault/panics/assert.h>
#endif

#include "cloud_module.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Handle fatal error.
 *  @param is_watchdog_timeout Boolean indicating if the macro was called upon a watchdog timeout.
 */
#define FATAL_ERROR_HANDLE(is_watchdog_timeout) do {				\
	enum error_type type = ERROR_FATAL;					\
	(void)zbus_chan_pub(&ERROR_CHAN, &type, K_SECONDS(10));			\
	LOG_PANIC();								\
	if (is_watchdog_timeout) {						\
		IF_ENABLED(CONFIG_MEMFAULT, (MEMFAULT_SOFTWARE_WATCHDOG()));	\
	}									\
	k_sleep(K_SECONDS(10));							\
	__ASSERT(false, "SEND_FATAL_ERROR() macro called");			\
} while (0)

/** @brief Macro used to handle fatal errors. */
#define SEND_FATAL_ERROR() FATAL_ERROR_HANDLE(0)
/** @brief Macro used to handle watchdog timeouts. */
#define SEND_FATAL_ERROR_WATCHDOG_TIMEOUT() FATAL_ERROR_HANDLE(1)

#define SEND_IRRECOVERABLE_ERROR() do {					\
	enum error_type type = ERROR_IRRECOVERABLE;				\
	(void)zbus_chan_pub(&ERROR_CHAN, &type, K_SECONDS(10));			\
	LOG_PANIC();								\
	k_sleep(K_SECONDS(10));							\
} while (0)

/** NETWORK_MODULE */

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
	 * attach to without finding a suitable cell accoring to 3GPP selection rules.
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



enum trigger_type {
	TRIGGER_POLL_SHADOW = 0x1,
	TRIGGER_FOTA_POLL,
	TRIGGER_DATA_SAMPLE,
};

#define MSG_TO_TRIGGER_TYPE(_msg)	(*(const enum trigger_type *)_msg)

enum trigger_mode {
	TRIGGER_MODE_POLL = 0x1,
	TRIGGER_MODE_NORMAL,
};

enum time_status {
	TIME_AVAILABLE = 0x1,
};

#define MSG_TO_TIME_STATUS(_msg)	(*(const enum time_status *)_msg)

enum location_status {
	LOCATION_SEARCH_STARTED = 0x1,
	LOCATION_SEARCH_DONE,
};

enum error_type {
	ERROR_FATAL = 0x1,
	ERROR_IRRECOVERABLE,
};

/** @brief Status sent from the FOTA module on the FOTA_STATUS_CHAN channel. */
enum fota_status {
	/* No FOTA job is ongoing */
	FOTA_STATUS_IDLE = 0x1,

	/* FOTA started. */
	FOTA_STATUS_START,

	/* FOTA stopped. */
	FOTA_STATUS_STOP,

	/* A firmware image has been downloaded and a reboot is required to apply it.
	 * The FOTA module will perform the reboot CONFIG_APP_FOTA_REBOOT_DELAY_SECONDS seconds
	 * after this status is sent.
	 */
	FOTA_STATUS_REBOOT_PENDING,
};

#define MSG_TO_FOTA_STATUS(_msg) (*(const enum fota_status *)_msg)

struct configuration {
	/* LED */
	int led_red;
	int led_green;
	int led_blue;
	bool led_present;
	bool led_red_present;
	bool led_green_present;
	bool led_blue_present;

	/* Configuration */
	bool gnss;
	uint64_t update_interval;
	bool config_present;
	bool gnss_present;
	bool update_interval_present;
};

#define MSG_TO_CONFIGURATION(_msg) ((const struct configuration *)_msg)

ZBUS_CHAN_DECLARE(
	BUTTON_CHAN,
	CONFIG_CHAN,
	ERROR_CHAN,
	FOTA_STATUS_CHAN,
	LED_CHAN,
	NETWORK_CHAN,
	TIME_CHAN,
	TRIGGER_CHAN,
	TRIGGER_MODE_CHAN,
	LOCATION_CHAN
);

#ifdef __cplusplus
}
#endif

#endif /* _MESSAGE_CHANNEL_H_ */
