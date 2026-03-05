#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

if(CONFIG_APP_NETWORK_NTN)
  target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/network_ntn.c)
else()
  target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/network.c)
endif()

target_sources_ifdef(CONFIG_APP_NETWORK_SHELL app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/network_shell.c)

target_include_directories(app PRIVATE .)
