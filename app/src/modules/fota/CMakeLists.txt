#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_sources_ifdef(CONFIG_APP_FOTA app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/fota.c)
target_include_directories(app PRIVATE .)
