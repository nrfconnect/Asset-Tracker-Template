#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/power.c)
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/fuel_gauge_state.c)
target_sources_ifdef(CONFIG_APP_POWER_SHELL app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/power_shell.c)
target_include_directories(app PRIVATE .)
