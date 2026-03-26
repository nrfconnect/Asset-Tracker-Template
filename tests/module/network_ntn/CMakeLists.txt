#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(network_ntn_module_test)

test_runner_generate(src/main.c)

target_sources(app
  PRIVATE
  src/main.c
  ../../../app/src/modules/network/network_ntn.c
)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(../../../app/src/common)
zephyr_include_directories(../../../app/src/modules/cloud)
zephyr_include_directories(../../../app/src/modules/network)
zephyr_include_directories(${NRF_BASE}/include/)
zephyr_include_directories(${ZEPHYR_BASE}/../nrfxlib/nrf_modem/include/)

zephyr_compile_definitions(UNITY_INCLUDE_DOUBLE)

target_link_options(app PRIVATE --whole-archive)

target_compile_definitions(app PRIVATE
	-DCONFIG_APP_NETWORK_NTN=1
	-DCONFIG_APP_NETWORK_NTN_LOG_LEVEL=4
	-DCONFIG_APP_NETWORK_NTN_THREAD_STACK_SIZE=2048
	-DCONFIG_APP_NETWORK_NTN_MSG_PROCESSING_TIMEOUT_SECONDS=1
	-DCONFIG_APP_NETWORK_NTN_WATCHDOG_TIMEOUT_SECONDS=2
	-DCONFIG_APP_NETWORK_NTN_TN_SEARCH_TIMEOUT_SECONDS=10
	-DCONFIG_APP_NETWORK_NTN_STATIC_LATITUDE=59930000
	-DCONFIG_APP_NETWORK_NTN_STATIC_LONGITUDE=10750000
	-DCONFIG_APP_NETWORK_NTN_STATIC_ALTITUDE=50
	-DCONFIG_APP_NETWORK_NTN_SEARCH_NETWORK_ON_STARTUP=1
	-DCONFIG_NTN=1
	-DCONFIG_APP_CLOUD_PAYLOAD_BUFFER_MAX_SIZE=100
	-DCONFIG_LTE_LC_NETWORK_REGISTRATION_MODULE=1
	-DCONFIG_LTE_LC_EDRX_MODULE=1
	-DCONFIG_LTE_LC_PSM_MODULE=1
	-DCONFIG_LTE_LC_PDN_MODULE=1
)
