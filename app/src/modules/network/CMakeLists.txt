#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_sources_ifdef(CONFIG_APP_NETWORK_BACKEND_LTE_LC
	app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/network.c)
target_sources_ifdef(CONFIG_APP_NETWORK_BACKEND_CONN_MGR
	app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/network_conn_mgr.c)
target_sources_ifdef(CONFIG_APP_NETWORK_SHELL
	app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/network_shell.c)
target_include_directories(app PRIVATE .)
