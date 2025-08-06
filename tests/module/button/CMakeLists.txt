#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(environmental_module_test)

test_runner_generate(src/button_module_test.c)

target_sources(app
  PRIVATE
  src/button_module_test.c
  ../../../app/src/modules/button/button.c
)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(../../../app/src/common)
zephyr_include_directories(../../../app/src/modules/cloud)
zephyr_include_directories(../../../app/src/modules/button)

target_link_options(app PRIVATE --whole-archive)
# Options that cannot be passed through Kconfig fragments
target_compile_definitions(app PRIVATE
	-DCONFIG_APP_CLOUD_PAYLOAD_BUFFER_MAX_SIZE=100
  -DCONFIG_APP_BUTTON_LOG_LEVEL=0
  -DCONFIG_APP_BUTTON_LONG_PRESS_TIMEOUT_MS=3000
)
