#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(shell_module_test)

test_runner_generate(src/main.c)

target_sources(app
  PRIVATE
  src/main.c
  src/shell_test.c
  ../../../app/src/modules/shell/shell.c
  ../../../app/src/common/message_channel.c
)

target_include_directories(app PRIVATE src)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(../../../app/src/common)
zephyr_include_directories(../../../app/src/modules/shell)

target_link_options(app PRIVATE --whole-archive)

# Options that cannot be passed through Kconfig fragments
target_compile_definitions(app PRIVATE
	-DCONFIG_APP_SHELL_LOG_LEVEL=4
	-DCONFIG_APP_PAYLOAD_CHANNEL_BUFFER_MAX_SIZE=100
)
