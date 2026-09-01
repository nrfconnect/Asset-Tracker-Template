#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

zephyr_include_directories(.)

if(CONFIG_APP_NTN_BACKEND_MYRIOTA)
	target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ntn_myriota.c)
else()
	target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ntn.c)
endif()

target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ntn_shell.c)
