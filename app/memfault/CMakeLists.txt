#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

if(CONFIG_MEMFAULT_NCS_FW_VERSION_STATIC AND
   NOT "${CONFIG_MEMFAULT_NCS_FW_VERSION}" STREQUAL "${APP_VERSION_STRING}")
    message(FATAL_ERROR
        "Memfault firmware version '${CONFIG_MEMFAULT_NCS_FW_VERSION}' "
        "does not match application version '${APP_VERSION_STRING}'. "
	"These values must match, use app/VERSION to set both.")
endif()
