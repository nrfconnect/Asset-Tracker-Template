#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

add_subdirectory(creds)
target_include_directories(app PRIVATE include)

target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cloud_mqtt.c)
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cloud_mqtt_location.c)
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cloud_mqtt_shell.c)
target_sources_ifdef(CONFIG_APP_ENVIRONMENTAL app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cloud_mqtt_environmental.c)

# Directly include FOTA header file to avoid build warning due to the modules being patched out
# due to not supported with the MQTT cloud example module.
target_include_directories(app PRIVATE ../../../app/src/modules/fota)
