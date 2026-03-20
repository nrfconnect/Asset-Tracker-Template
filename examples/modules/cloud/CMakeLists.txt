#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

add_subdirectory(creds)
target_include_directories(app PRIVATE include)

target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cloud_mqtt.c)
target_sources_ifdef(CONFIG_APP_CLOUD_MQTT_SHELL app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cloud_mqtt_shell.c)
