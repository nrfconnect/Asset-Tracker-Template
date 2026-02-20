#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_module_test)

test_runner_generate(src/main.c)

set(ASSET_TRACKER_TEMPLATE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

# Make Kconfig values available as CMake variables for CBOR generation
set(CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE 10)
# Include CBOR generation (required by main app)
add_subdirectory(${ASSET_TRACKER_TEMPLATE_DIR}/app/src/cbor ${CMAKE_CURRENT_BINARY_DIR}/cbor)
target_sources(app
	PRIVATE
	src/main.c
	src/checks.c
	${ASSET_TRACKER_TEMPLATE_DIR}/app/src/main.c
	${ASSET_TRACKER_TEMPLATE_DIR}/app/src/cbor/cbor_helper.c
)

target_include_directories(app PRIVATE src)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(../../../app/src/common)
zephyr_include_directories(../../../app/src/cbor)
zephyr_include_directories(../../../app/src/modules/cloud)
zephyr_include_directories(../../../app/src/modules/power)
zephyr_include_directories(../../../app/src/modules/button)
zephyr_include_directories(../../../app/src/modules/network)
zephyr_include_directories(../../../app/src/modules/environmental)
zephyr_include_directories(../../../app/src/modules/fota)
zephyr_include_directories(../../../app/src/modules/location)
zephyr_include_directories(../../../app/src/modules/led)
zephyr_include_directories(../../../app/src/modules/storage)
zephyr_include_directories(${ZEPHYR_BASE}/../nrfxlib/nrf_modem/include)

target_link_options(app PRIVATE --whole-archive)

add_compile_options(-Wno-return-type)

set_property(SOURCE ${ASSET_TRACKER_TEMPLATE_DIR}/app/src/main.c PROPERTY COMPILE_FLAGS
	     "-include ${CMAKE_CURRENT_SOURCE_DIR}/src/redef.h")

# Options that cannot be passed through Kconfig fragments
target_compile_definitions(app PRIVATE
	-DCONFIG_APP_LOG_LEVEL=1
	-DCONFIG_APP_MSG_PROCESSING_TIMEOUT_SECONDS=120
	-DCONFIG_APP_WATCHDOG_TIMEOUT_SECONDS=180
	-DCONFIG_APP_CLOUD_PAYLOAD_BUFFER_MAX_SIZE=100
	-DCONFIG_APP_CLOUD_SHADOW_RESPONSE_BUFFER_MAX_SIZE=512
	-DCONFIG_APP_SAMPLING_INTERVAL_SECONDS=600
	-DCONFIG_APP_CLOUD_UPDATE_INTERVAL_SECONDS=1500
	-DCONFIG_APP_CLOUD_LOG_LEVEL=0
	-DCONFIG_APP_CLOUD_THREAD_STACK_SIZE=2048
	-DCONFIG_APP_CLOUD_MESSAGE_QUEUE_SIZE=5
	-DCONFIG_APP_CLOUD_BACKOFF_TYPE_LINEAR=1
	-DCONFIG_APP_CLOUD_BACKOFF_INITIAL_SECONDS=6
	-DCONFIG_APP_CLOUD_BACKOFF_LINEAR_INCREMENT_SECONDS=6
	-DCONFIG_APP_CLOUD_BACKOFF_MAX_SECONDS=36
	-DCONFIG_LTE_LC_EDRX_MODULE=1
	-DCONFIG_LTE_LC_PSM_MODULE=1
	-DCOAP_CONTENT_FORMAT_APP_JSON=50
	-DCONFIG_APP_POWER=1
	-DCONFIG_APP_ENVIRONMENTAL=1
	-DZEPHYR_INCLUDE_SYS_REBOOT_H_
	-DCONFIG_LOCATION_SERVICE_EXTERNAL
	-DCONFIG_LOCATION_METHOD_CELLULAR=1
	-DCONFIG_LOCATION_METHOD_WIFI=1
	-DCONFIG_LOCATION_METHODS_LIST_SIZE=3
	-DCONFIG_APP_LOCATION_WIFI_APS_MAX=10
	-DCONFIG_APP_LOCATION_NEIGHBOR_CELLS_MAX=10
	-DCONFIG_LTE_NEIGHBOR_CELLS_MAX=10
	-DCONFIG_LOCATION_METHOD_WIFI_SCANNING_RESULTS_MAX_CNT=10
	-DCONFIG_NRF_CLOUD_AGNSS=y
	-DCONFIG_APP_STORAGE_INITIAL_THRESHOLD=1
)

