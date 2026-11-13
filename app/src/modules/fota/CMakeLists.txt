#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/fota.c)
target_sources_ifdef(CONFIG_APP_FOTA_SHELL app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/fota_shell.c)
if(CONFIG_APP_FOTA_MCUBOOT OR CONFIG_APP_FOTA_MODEM_FULL)
	target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/fota_components.c)
endif()
target_include_directories(app PRIVATE .)
