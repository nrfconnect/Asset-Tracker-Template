#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

zephyr_include_directories(.)
if(CONFIG_SOFTSIM)
zephyr_include_directories(${ZEPHYR_BASE}/../nrf/modules/memfault-firmware-sdk/include)
endif()
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ntn.c)
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ntn_shell.c)
