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
  ../../../app/src/modules/cloud/cloud.c
  ../../../app/src/modules/cloud/cloud_provisioning.c
  ../../../app/src/modules/cloud/cloud_location.c
  ../../../app/src/modules/cloud/cloud_environmental.c
)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/net)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(../../../app/src/modules/cloud)
zephyr_include_directories(../../../app/src/common)
zephyr_include_directories(../../../app/src/modules/power)
zephyr_include_directories(../../../app/src/modules/network)
zephyr_include_directories(../../../app/src/modules/environmental)
zephyr_include_directories(../../../app/src/modules/location)
zephyr_include_directories(../../../app/src/modules/storage)
zephyr_include_directories(${NRF_DIR}/subsys/net/lib/nrf_cloud/include)
zephyr_include_directories(${NRF_DIR}/subsys/net/lib/nrf_cloud/common/include)
zephyr_include_directories(${NRF_DIR}/include/net)
zephyr_include_directories(${NRF_DIR}/subsys/net/lib/nrf_cloud/coap/include)
zephyr_include_directories(${NRF_DIR}/../modules/lib/cjson)
zephyr_include_directories(${ZEPHYR_BASE}/../nrfxlib/nrf_modem/include)

target_link_options(app PRIVATE --whole-archive)

# Options that cannot be passed through Kconfig fragments
target_compile_definitions(app PRIVATE
	-DCONFIG_APP_CLOUD_PAYLOAD_BUFFER_MAX_SIZE=256
	-DCONFIG_APP_CLOUD_SHADOW_RESPONSE_BUFFER_MAX_SIZE=512
	-DCONFIG_APP_CLOUD_LOG_LEVEL=4
	-DCONFIG_APP_CLOUD_THREAD_STACK_SIZE=2048
	-DCONFIG_APP_CLOUD_MESSAGE_QUEUE_SIZE=5
	-DCONFIG_APP_CLOUD_MSG_PROCESSING_TIMEOUT_SECONDS=1
	-DCONFIG_APP_CLOUD_WATCHDOG_TIMEOUT_SECONDS=2
	-DCONFIG_APP_CLOUD_BACKOFF_TYPE_LINEAR=1
	-DCONFIG_APP_CLOUD_BACKOFF_INITIAL_SECONDS=6
	-DCONFIG_APP_CLOUD_BACKOFF_LINEAR_INCREMENT_SECONDS=6
	-DCONFIG_APP_CLOUD_BACKOFF_MAX_SECONDS=36
	-DCONFIG_APP_POWER=1
	-DCONFIG_APP_ENVIRONMENTAL=1
	-DCONFIG_LTE_LC_CONN_EVAL_MODULE=1
	-DCONFIG_LTE_LC_EDRX_MODULE=1
	-DCONFIG_LTE_LC_PSM_MODULE=1
	-DCONFIG_COAP_CONTENT_FORMAT_APP_JSON=50
	-DCONFIG_NRF_CLOUD_COAP=1
	-DCONFIG_COAP_CLIENT_MESSAGE_HEADER_SIZE=1024
	-DCONFIG_COAP_CLIENT_MESSAGE_SIZE=1024
	-DCONFIG_COAP_CLIENT_MAX_REQUESTS=5
	-DCONFIG_COAP_CLIENT_BLOCK_SIZE=1024
	-DCONFIG_APP_LOCATION=1
	-DCONFIG_APP_LOCATION_WIFI_APS_MAX=10
	-DCONFIG_APP_LOCATION_NEIGHBOR_CELLS_MAX=10
	-DCONFIG_LTE_NEIGHBOR_CELLS_MAX=10
	-DCONFIG_LOCATION_METHOD_WIFI_SCANNING_RESULTS_MAX_CNT=10
	-DCONFIG_LOCATION_SERVICE_EXTERNAL
	-DCONFIG_LOCATION_METHOD_CELLULAR=1
	-DCONFIG_LOCATION_METHOD_WIFI=1
	-DCONFIG_LOCATION_METHOD_GNSS=1
	-DCONFIG_LOCATION_METHODS_LIST_SIZE=3
	-DCONFIG_NRF_CLOUD_AGNSS=y
)
