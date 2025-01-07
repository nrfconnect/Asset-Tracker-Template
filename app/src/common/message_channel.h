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

struct payload {
	uint8_t buffer[CONFIG_APP_PAYLOAD_CHANNEL_BUFFER_MAX_SIZE];
	size_t buffer_len;
};

#define MSG_TO_PAYLOAD(_msg) ((struct payload *)_msg)

enum network_msg_type {
	/* Output message types */
	NETWORK_DISCONNECTED = 0x1,
	NETWORK_CONNECTED,
	NETWORK_MODEM_RESET_LOOP,
	NETWORK_UICC_FAILURE,
	NETWORK_LIGHT_SERACH_DONE,
	NETWORK_ATTACH_REJECTED,
	NETWORK_PSM_PARAMS,
	NETWORK_EDRX_PARAMS,
	NETWORK_SYSTEM_MODE_RESPONSE,
	NETWORK_QUALITY_SAMPLE_RESPONSE,

	/* Input message types */
	NETWORK_CONNECT,
	NETWORK_DISCONNECT,
	NETWORK_SEARCH_STOP,
	NETWORK_SEARCH_START,
	NETWORK_QUALITY_SAMPLE_REQUEST,
	NETWORK_SYSTEM_MODE_SET_LTE_M,
	NETWORK_SYSTEM_MODE_SET_NBIOT,
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

#define MSG_TO_NETWORK_STATUS(_msg)	(*(const enum network_msg_type *)_msg)

enum cloud_status {
	CLOUD_DISCONNECTED = 0x1,
	CLOUD_CONNECTED_READY_TO_SEND,
	CLOUD_CONNECTED_PAUSED,
};

#define MSG_TO_CLOUD_STATUS(_msg)	(*(const enum cloud_status *)_msg)

enum trigger_type {
	TRIGGER_POLL = 0x1,
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
	CLOUD_CHAN,
	CONFIG_CHAN,
	ERROR_CHAN,
	FOTA_STATUS_CHAN,
	LED_CHAN,
	NETWORK_CHAN,
	PAYLOAD_CHAN,
	TIME_CHAN,
	TRIGGER_CHAN,
	TRIGGER_MODE_CHAN,
	LOCATION_CHAN
);

#ifdef __cplusplus
}
#endif

#endif /* _MESSAGE_CHANNEL_H_ */
