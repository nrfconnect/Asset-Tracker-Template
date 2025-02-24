#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cloud_module_test)

test_runner_generate(src/cloud_module_test.c)

target_sources(app
  PRIVATE
  src/cloud_module_test.c
  ../../../app/src/modules/cloud/cloud_module.c
  ../../../app/src/common/message_channel.c
)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(../../../app/src/modules/cloud)
zephyr_include_directories(../../../app/src/common)
zephyr_include_directories(../../../app/src/modules/battery)
zephyr_include_directories(../../../app/src/modules/network)
zephyr_include_directories(../../../app/src/modules/environmental)
zephyr_include_directories(${NRF_DIR}/subsys/net/lib/nrf_cloud/include)
zephyr_include_directories(${NRF_DIR}/../modules/lib/cjson)

target_link_options(app PRIVATE --whole-archive)

# Options that cannot be passed through Kconfig fragments
target_compile_definitions(app PRIVATE
	-DCONFIG_APP_PAYLOAD_CHANNEL_BUFFER_MAX_SIZE=100
	-DCONFIG_APP_CLOUD_LOG_LEVEL=0
	-DCONFIG_APP_CLOUD_THREAD_STACK_SIZE=2048
	-DCONFIG_APP_CLOUD_MESSAGE_QUEUE_SIZE=5
	-DCONFIG_APP_CLOUD_MSG_PROCESSING_TIMEOUT_SECONDS=1
	-DCONFIG_APP_CLOUD_WATCHDOG_TIMEOUT_SECONDS=2
	-DCONFIG_APP_CLOUD_BACKOFF_TYPE_LINEAR=1
	-DCONFIG_APP_CLOUD_BACKOFF_INITIAL_SECONDS=6
	-DCONFIG_APP_CLOUD_BACKOFF_LINEAR_INCREMENT_SECONDS=6
	-DCONFIG_APP_CLOUD_BACKOFF_MAX_SECONDS=36
	-DCONFIG_LTE_LC_CONN_EVAL_MODULE=1
	-DCONFIG_LTE_LC_EDRX_MODULE=1
	-DCONFIG_LTE_LC_PSM_MODULE=1
	-DCONFIG_APP_MODULE_RECV_BUFFER_SIZE=1024
	-DCOAP_CONTENT_FORMAT_APP_JSON=50
)
