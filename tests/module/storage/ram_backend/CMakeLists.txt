#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(storage_module_test)

test_runner_generate(../src/storage_module_test.c)

target_sources(app
	PRIVATE
	../src/storage_module_test.c
	../src/test_samples.c
	../../../../app/src/modules/storage/storage.c
	../../../../app/src/modules/storage/storage_data_types.c
	../../../../app/src/modules/storage/backends/ram_ring_buffer_backend.c
)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(../../../../app/src/modules/storage)
zephyr_include_directories(../../../../app/src/modules/storage/backends)
zephyr_include_directories(../../../../app/src/common)
zephyr_include_directories(../../../../app/src/modules/power)
zephyr_include_directories(../../../../app/src/modules/network)
zephyr_include_directories(../../../../app/src/modules/environmental)
zephyr_include_directories(../../../../app/src/modules/location)
zephyr_include_directories(${ZEPHYR_BASE}/../nrfxlib/nrf_modem/include)
zephyr_include_directories(../src)

zephyr_linker_sources(SECTIONS ../../../../app/src/modules/storage/storage_sections.ld)

# The test uses double precision floating point numbers. This is not enabled by default in unity
# unless we set the following define.
zephyr_compile_definitions(UNITY_INCLUDE_DOUBLE)

# Options that cannot be passed through Kconfig fragments
target_compile_definitions(app PRIVATE
	-DCONFIG_APP_STORAGE_INITIAL_MODE_BUFFER=1
	-DCONFIG_APP_STORAGE_MAX_TYPES=4
	-DCONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE=100
	-DCONFIG_APP_STORAGE_MESSAGE_QUEUE_SIZE=10
	-DCONFIG_APP_STORAGE_THREAD_STACK_SIZE=1720
	-DCONFIG_APP_STORAGE_WATCHDOG_TIMEOUT_SECONDS=120
	-DCONFIG_APP_STORAGE_MSG_PROCESSING_TIMEOUT_SECONDS=60
	-DCONFIG_APP_POWER=1
	-DCONFIG_APP_ENVIRONMENTAL=1
	-DCONFIG_APP_STORAGE_LOG_LEVEL=3
	-DCONFIG_APP_STORAGE_BATCH_BUFFER_SIZE=1024
	-DCONFIG_APP_STORAGE_SESSION_TIMEOUT_SECONDS=60
	-DCONFIG_APP_LOCATION=1
	-DCONFIG_APP_LOCATION_WIFI_APS_MAX=10
	-DCONFIG_APP_LOCATION_NEIGHBOR_CELLS_MAX=10
	-DCONFIG_LTE_NEIGHBOR_CELLS_MAX=10
	-DCONFIG_LOCATION_METHOD_WIFI_SCANNING_RESULTS_MAX_CNT=10
	-DCONFIG_LOCATION_SERVICE_EXTERNAL
	-DCONFIG_LOCATION_METHOD_CELLULAR=1
	-DCONFIG_LOCATION_METHOD_WIFI=1
	-DCONFIG_LOCATION_METHODS_LIST_SIZE=3
	-DCONFIG_LTE_LC_CONN_EVAL_MODULE=1
)
