#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/button.c)
target_sources_ifdef(CONFIG_APP_BUTTON_SHELL app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/button_shell.c)
target_include_directories(app PRIVATE .)
