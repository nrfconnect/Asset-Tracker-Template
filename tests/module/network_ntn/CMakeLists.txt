#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(network_ntn_module_test)

test_runner_generate(src/network_ntn_module_test.c)

target_sources(app
  PRIVATE
  src/network_ntn_module_test.c
  ../../../app/src/modules/network/network_ntn.c
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
	-DCONFIG_APP_NETWORK_THREAD_STACK_SIZE=1536
	-DCONFIG_APP_NETWORK_MESSAGE_QUEUE_SIZE=5
	-DCONFIG_APP_NETWORK_MSG_PROCESSING_TIMEOUT_SECONDS=1
	-DCONFIG_APP_NETWORK_WATCHDOG_TIMEOUT_SECONDS=2
	-DCONFIG_APP_NETWORK_NTN=1
	-DCONFIG_APP_NETWORK_NTN_PERIODIC_TN_SEARCH_INTERVAL_SECONDS=3600
	-DCONFIG_APP_NETWORK_NTN_TLE_STALE_HOURS=168
	-DCONFIG_NTN=1
	-DCONFIG_LTE_LC_NETWORK_REGISTRATION_MODULE=1
	-DCONFIG_LTE_LC_EDRX_MODULE=1
	-DCONFIG_LTE_LC_PSM_MODULE=1
	-DCONFIG_LTE_LC_PDN_MODULE=1
	-DCONFIG_LTE_NETWORK_MODE_LTE_M_NBIOT_GPS=1
	-DCONFIG_APP_NETWORK_TN_SIM_TYPE=0
	-DCONFIG_APP_NETWORK_NTN_SIM_TYPE=0
)
