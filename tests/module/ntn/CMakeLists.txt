#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sgp4_pass_test)

test_runner_generate(src/sgp4_pass_test.c)

target_sources(app
  PRIVATE
  src/sgp4_pass_test.c
  ../../../app/src/modules/sgp4/SGP4.c

)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(../../../app/src/common)
zephyr_include_directories(../../../app/src/modules/sgp4)

target_link_options(app PRIVATE --whole-archive)
