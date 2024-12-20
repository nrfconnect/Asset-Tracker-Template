#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project("Asset Tracker Template")

# Include files that are common for all modules
add_subdirectory(src/common)

# Include mandatory module source folders
add_subdirectory(src/modules/trigger)
add_subdirectory(src/modules/battery)
add_subdirectory(src/modules/network)
add_subdirectory(src/modules/transport)
add_subdirectory(src/modules/location)
add_subdirectory(src/modules/app)
add_subdirectory(src/modules/fota)
add_subdirectory(src/modules/environmental)
add_subdirectory(src/modules/button)

# Optional modules
add_subdirectory_ifdef(CONFIG_APP_LED src/modules/led)
add_subdirectory_ifdef(CONFIG_APP_MEMFAULT src/modules/memfault)
add_subdirectory_ifdef(CONFIG_APP_SHELL src/modules/shell)

# Include Memfault configuration folder
zephyr_include_directories(config)

if (CONFIG_NRF_CLOUD_COAP_SEC_TAG GREATER_EQUAL 2147483648 AND CONFIG_NRF_CLOUD_COAP_SEC_TAG LESS_EQUAL 2147483667)
	message(WARNING "CONFIG_NRF_CLOUD_COAP_SEC_TAG is set to a developer security tag. "
			"TLS traffic can now be decrypted with Nordic tools. "
			"This should only be used during development and testing.")
endif()
