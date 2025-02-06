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

enum time_status {
	TIME_AVAILABLE = 0x1,
};

#define MSG_TO_TIME_STATUS(_msg)	(*(const enum time_status *)_msg)

enum error_type {
	ERROR_FATAL = 0x1,
	ERROR_IRRECOVERABLE,
};

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
	CONFIG_CHAN,
	ERROR_CHAN,
	TIME_CHAN
);

#ifdef __cplusplus
}
#endif

#endif /* _MESSAGE_CHANNEL_H_ */
