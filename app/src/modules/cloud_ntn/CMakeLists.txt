#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cloud.c)
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cloud_shell.c)
target_include_directories(app PRIVATE .)
