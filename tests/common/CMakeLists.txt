#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_common_test)

test_runner_generate(src/app_common_test.c)

target_sources(app
  PRIVATE
  src/app_common_test.c
)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(..//../app/src/common)

target_link_options(app PRIVATE --whole-archive)

# Options that cannot be passed through Kconfig fragments
target_compile_definitions(app PRIVATE
	-DCONFIG_ASSERT=1
	-DCONFIG_ASSERT_LEVEL=2
	-DCONFIG_ASSERT_TEST=1
)
