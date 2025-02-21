#
# Copyright (c) 2023 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#
menu "App"

config APP_MODULE_TRIGGER_TIMEOUT_SECONDS
	int "Trigger timer timeout"
	default 600
	help
	  Timeout for the trigger timer. On timeout, the module will send triggers for
	  sensor sampling, location search and polling of shadow and FOTA status from cloud.

config APP_MODULE_THREAD_STACK_SIZE
	int "Thread stack size"
	default 3200

config APP_MODULE_WATCHDOG_TIMEOUT_SECONDS
	int "Watchdog timeout seconds"
	default 330

config APP_MODULE_EXEC_TIME_SECONDS_MAX
	int "Maximum execution time seconds"
	default 270
	help
	  Maximum time allowed for a single execution of the module thread loop.

config APP_MODULE_RECV_BUFFER_SIZE
	int "Receive buffer size"
	default 1024

config APP_REQUEST_NETWORK_QUALITY
	bool "Request network quality"
	help
	  Request network quality on triggers.

module = APP
module-str = APP
source "subsys/logging/Kconfig.template.log_config"

endmenu
