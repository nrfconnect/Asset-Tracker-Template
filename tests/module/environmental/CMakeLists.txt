#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(environmental_module_test)

test_runner_generate(src/environmental_module_test.c)

set(ASSET_TRACKER_TEMPLATE_DIR ../../..)

target_sources(app
  PRIVATE
  src/environmental_module_test.c
  src/redef.c
  ../../../app/src/modules/environmental/environmental.c
)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(../../../app/src/common)
zephyr_include_directories(../../../app/src/modules/cloud)
zephyr_include_directories(../../../app/src/modules/environmental)

target_link_options(app PRIVATE --whole-archive)
# Options that cannot be passed through Kconfig fragments
target_compile_definitions(app PRIVATE
	-DCONFIG_APP_CLOUD_PAYLOAD_BUFFER_MAX_SIZE=100
	-DCONFIG_APP_ENVIRONMENTAL_LOG_LEVEL=4
	-DCONFIG_APP_ENVIRONMENTAL_THREAD_STACK_SIZE=1024
	-DCONFIG_APP_ENVIRONMENTAL_MESSAGE_QUEUE_SIZE=5
	-DCONFIG_APP_ENVIRONMENTAL_MSG_PROCESSING_TIMEOUT_SECONDS=1
	-DCONFIG_APP_ENVIRONMENTAL_WATCHDOG_TIMEOUT_SECONDS=2
	-DCONFIG_APP_ENVIRONMENTAL_TIMESTAMP=y
)

set_property(SOURCE ${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/environmental/environmental.c PROPERTY COMPILE_FLAGS
             "-include ${CMAKE_CURRENT_SOURCE_DIR}/src/redef.h")
