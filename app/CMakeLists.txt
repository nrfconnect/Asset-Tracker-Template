#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project("Asset Tracker Template")

# Include files that are common for all modules
target_include_directories(app PRIVATE src/common)

zephyr_include_directories(config)

# Add main application source

# Module source folders
add_subdirectory(src/modules/button)
add_subdirectory(src/cbor)

# Optional modules
add_subdirectory_ifdef(CONFIG_APP_POWER src/modules/power)
add_subdirectory_ifdef(CONFIG_APP_ENVIRONMENTAL src/modules/environmental)
add_subdirectory_ifdef(CONFIG_APP_LED src/modules/led)
if(CONFIG_APP_NTN_MODE)
	add_subdirectory(src/modules/ntn_module)
else()
	target_sources(app PRIVATE src/main.c)
	add_subdirectory(src/modules/network)
	add_subdirectory_ifdef(CONFIG_APP_CLOUD src/modules/cloud)
	add_subdirectory_ifdef(CONFIG_APP_LOCATION src/modules/location)
endif()
add_subdirectory_ifdef(CONFIG_APP_FOTA src/modules/fota)
