#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(fota_module_test)

test_runner_generate(src/fota_module_test.c)

set(ASSET_TRACKER_TEMPLATE_DIR ../../..)

target_sources(app
	PRIVATE
	src/fota_module_test.c
	${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/fota/fota.c
)

zephyr_include_directories(${ASSET_TRACKER_TEMPLATE_DIR}/app/src/common)
zephyr_include_directories(${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/fota)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)

zephyr_include_directories(${NRF_DIR}/include/net)
zephyr_include_directories(${NRF_DIR}/subsys/net/lib/nrf_cloud/include)
zephyr_include_directories(${NRF_DIR}/subsys/net/lib/nrf_cloud/common/include)
zephyr_include_directories(${NRF_DIR}/subsys/net/lib/nrf_cloud/mqtt/include)
zephyr_include_directories(${NRF_DIR}/../modules/lib/cjson)
zephyr_include_directories(${NRF_DIR}/../modules/lib/cjson)

target_link_options(app PRIVATE --whole-archive)

# Options that cannot be passed through Kconfig fragments
target_compile_definitions(app PRIVATE
	-DCONFIG_APP_CLOUD_PAYLOAD_BUFFER_MAX_SIZE=100
	-DCONFIG_APP_FOTA_LOG_LEVEL=0
	-DCONFIG_APP_FOTA_THREAD_STACK_SIZE=1536
	-DCONFIG_APP_FOTA_MESSAGE_QUEUE_SIZE=5
	-DCONFIG_APP_FOTA_MSG_PROCESSING_TIMEOUT_SECONDS=180
	-DCONFIG_APP_FOTA_WATCHDOG_TIMEOUT_SECONDS=210
	-DCONFIG_DOWNLOADER_MAX_HOSTNAME_SIZE=128
	-DCONFIG_DOWNLOADER_MAX_FILENAME_SIZE=256
	-DCONFIG_DOWNLOADER_TRANSPORT_PARAMS_SIZE=64
	-DCONFIG_DOWNLOADER_STACK_SIZE=4096
	-DCONFIG_NRF_CLOUD_COAP=y
	-DCONFIG_COAP_CLIENT_MESSAGE_HEADER_SIZE=64
	-DCONFIG_COAP_CLIENT_MESSAGE_SIZE=1024
	-DCONFIG_COAP_CLIENT_MAX_REQUESTS=5
	-DCONFIG_COAP_CLIENT_BLOCK_SIZE=64
)
