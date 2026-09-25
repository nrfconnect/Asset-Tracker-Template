#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

zephyr_include_directories(include)

add_subdirectory(lib)
add_subdirectory_ifdef(CONFIG_APP_CLOUD_MQTT examples/modules/cloud/)
