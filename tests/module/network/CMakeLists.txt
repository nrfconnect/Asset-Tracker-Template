#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(network_module_test)

test_runner_generate(src/main.c)

target_sources(app
  PRIVATE
  src/main.c
  ../../../app/src/modules/network/network.c
)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(../../../app/src/common)
zephyr_include_directories(../../../app/src/modules/cloud)
zephyr_include_directories(../../../app/src/modules/network)
zephyr_include_directories(${NRF_BASE}/include/)
zephyr_include_directories(${ZEPHYR_BASE}/../nrfxlib/nrf_modem/include/)

target_link_options(app PRIVATE --whole-archive)
# Options that cannot be passed through Kconfig fragments
target_compile_definitions(app PRIVATE
	-DCONFIG_APP_CLOUD_PAYLOAD_BUFFER_MAX_SIZE=100
	-DCONFIG_APP_NETWORK_LOG_LEVEL=4
	-DCONFIG_APP_NETWORK_THREAD_STACK_SIZE=1024
	-DCONFIG_APP_NETWORK_MESSAGE_QUEUE_SIZE=5
	-DCONFIG_APP_NETWORK_MSG_PROCESSING_TIMEOUT_SECONDS=1
	-DCONFIG_APP_NETWORK_WATCHDOG_TIMEOUT_SECONDS=2
	-DCONFIG_LTE_LC_NETWORK_REGISTRATION_MODULE=1
	-DCONFIG_LTE_LC_EDRX_MODULE=1
	-DCONFIG_LTE_LC_PSM_MODULE=1
	-DCONFIG_LTE_LC_PDN_MODULE=1
)
