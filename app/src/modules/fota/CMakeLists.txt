#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/fota.c)
target_sources_ifdef(CONFIG_APP_FOTA_SHELL app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/fota_shell.c)
target_include_directories(app PRIVATE .)
