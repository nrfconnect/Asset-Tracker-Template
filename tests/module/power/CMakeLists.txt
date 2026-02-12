#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(power_module_test)

test_runner_generate(src/power_module_test.c)

set(ASSET_TRACKER_TEMPLATE_DIR ../../..)

target_sources(app
  PRIVATE
  src/power_module_test.c
  src/redef.c
  ${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/power/power.c
  ${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/power/fuel_gauge_state.c
)

target_include_directories(app PRIVATE src)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/drivers/mfd/)
zephyr_include_directories(${ZEPHYR_NRFXLIB_MODULE_DIR}/nrf_fuel_gauge/include/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(${ZEPHYR_BASE}/../nrfxlib/nrf_modem/include)
zephyr_include_directories(${ASSET_TRACKER_TEMPLATE_DIR}/app/src/common)
zephyr_include_directories(${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/cloud)
zephyr_include_directories(${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/power)

# Options that cannot be passed through Kconfig fragments
target_compile_definitions(app PRIVATE
	-DCONFIG_APP_POWER_LOG_LEVEL=4
	-DCONFIG_APP_POWER_THREAD_STACK_SIZE=4096
	-DCONFIG_APP_CLOUD_PAYLOAD_BUFFER_MAX_SIZE=100
  -DCONFIG_APP_POWER_MSG_PROCESSING_TIMEOUT_SECONDS=3
  -DCONFIG_APP_POWER_WATCHDOG_TIMEOUT_SECONDS=120
)

set_property(SOURCE ${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/power/power.c PROPERTY COMPILE_FLAGS
             "-include ${CMAKE_CURRENT_SOURCE_DIR}/src/redef.h")
