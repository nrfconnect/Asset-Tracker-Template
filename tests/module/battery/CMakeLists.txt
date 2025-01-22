#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(battery_module_test)

test_runner_generate(src/main.c)

set(ASSET_TRACKER_TEMPLATE_DIR ../../..)

target_sources(app
  PRIVATE
  src/main.c
  src/redef.c
  ${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/battery/battery.c
  ${ASSET_TRACKER_TEMPLATE_DIR}/app/src/common/message_channel.c
)

target_include_directories(app PRIVATE src)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_NRFXLIB_MODULE_DIR}/nrf_fuel_gauge/include/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(${ASSET_TRACKER_TEMPLATE_DIR}/app/src/common)
zephyr_include_directories(${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/cloud)

# Options that cannot be passed through Kconfig fragments
target_compile_definitions(app PRIVATE
	-DCONFIG_APP_BATTERY_LOG_LEVEL=4
	-DCONFIG_APP_BATTERY_THREAD_STACK_SIZE=4096
	-DCONFIG_APP_PAYLOAD_CHANNEL_BUFFER_MAX_SIZE=100
  -DCONFIG_APP_BATTERY_EXEC_TIME_SECONDS_MAX=3
  -DCONFIG_APP_BATTERY_WATCHDOG_TIMEOUT_SECONDS=120
)

set_property(SOURCE ${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/battery/battery.c PROPERTY COMPILE_FLAGS
             "-include ${CMAKE_CURRENT_SOURCE_DIR}/src/redef.h")
