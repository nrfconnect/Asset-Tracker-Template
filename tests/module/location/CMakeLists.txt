#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(location_module_test)

test_runner_generate(src/location_module_test.c)

set(ASSET_TRACKER_TEMPLATE_DIR ../../..)

target_sources(app
  PRIVATE
  src/location_module_test.c
  ../../../app/src/modules/location/location.c
)

zephyr_include_directories(${ASSET_TRACKER_TEMPLATE_DIR}/app/src/common)
zephyr_include_directories(${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/location)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/net)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(${ZEPHYR_BASE}/../nrfxlib/nrf_modem/include)

# The test uses double precision floating point numbers. This is not enabled by default in unity
# unless we set the following define.
zephyr_compile_definitions(UNITY_INCLUDE_DOUBLE)

target_link_options(app PRIVATE --whole-archive)

# Options that cannot be passed through Kconfig fragments
target_compile_definitions(app PRIVATE
  -DCONFIG_APP_LOCATION_LOG_LEVEL=4
  -DCONFIG_APP_LOCATION_THREAD_STACK_SIZE=2048
  -DCONFIG_APP_LOCATION_WATCHDOG_TIMEOUT_SECONDS=120
  -DCONFIG_APP_LOCATION_MSG_PROCESSING_TIMEOUT_SECONDS=60
  -DCONFIG_LOCATION_METHODS_LIST_SIZE=3
  -DCONFIG_LOCATION=1
  -DCONFIG_LOCATION_DATA_DETAILS=1
  -DCONFIG_LOCATION_METHOD_GNSS=1
  -DCONFIG_LOCATION_METHOD_CELLULAR=1
  -DCONFIG_LOCATION_METHOD_WIFI=1
  -DCONFIG_LOCATION_SERVICE_EXTERNAL=1
  -DCONFIG_LOCATION_SERVICE_NRF_CLOUD_GNSS_POS_SEND=1
  -DCONFIG_LOCATION_REQUEST_DEFAULT_GNSS_TIMEOUT=60000
  -DCONFIG_LOCATION_REQUEST_DEFAULT_CELLULAR_TIMEOUT=30000
  -DCONFIG_LOCATION_REQUEST_DEFAULT_TIMEOUT=120000
  -DCONFIG_LOCATION_REQUEST_DEFAULT_CELLULAR_CELL_COUNT=3
  -DCONFIG_LOCATION_REQUEST_DEFAULT_GNSS_VISIBILITY_DETECTION=1
  -DCONFIG_NRF_CLOUD_AGNSS=1
)
