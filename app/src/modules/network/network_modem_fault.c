/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <modem/nrf_modem_lib.h>
#if defined(CONFIG_NRF_MODEM_LIB_TRACE)
#include <modem/nrf_modem_lib_trace.h>
#endif

#include "app_common.h"

LOG_MODULE_DECLARE(network, CONFIG_APP_NETWORK_LOG_LEVEL);

/* Forward declarations */
static void modem_fault_work_fn(struct k_work *work);

static K_WORK_DEFINE(modem_fault_work, modem_fault_work_fn);

static void trigger_fatal_after_trace_flush(void)
{
#if defined(CONFIG_NRF_MODEM_LIB_TRACE)
	/* The modem emits its coredump asynchronously over the trace interface after
	 * a fault. Wait for the trace thread to flush it to the backend before crashing
	 * the application, so Memfault can upload the trace on the next boot.
	 */
	(void)nrf_modem_lib_trace_processing_done_wait(K_SECONDS(5));
#endif

	SEND_FATAL_ERROR();
}

static void modem_fault_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	trigger_fatal_after_trace_flush();
}

void nrf_modem_fault_handler(struct nrf_modem_fault_info *fault)
{
	LOG_ERR("Modem has crashed, reason 0x%x %s, PC: 0x%x",
		fault->reason, nrf_modem_lib_fault_strerror(fault->reason), fault->program_counter);

	int err = k_work_submit(&modem_fault_work);

	if (err < 0) {
		LOG_ERR("Work item was not submitted to system work queue, error %d", err);
		SEND_FATAL_ERROR();
	}
}
