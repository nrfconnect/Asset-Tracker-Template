#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cloud_mqtt_test)

test_runner_generate(src/cloud_mqtt_test.c)

target_sources(app
  PRIVATE
  src/cloud_mqtt_test.c
  ../../../examples/modules/cloud/cloud_mqtt.c
)

target_include_directories(app PRIVATE ../../../examples/modules/cloud/include)
target_include_directories(app PRIVATE ../../../app/src/modules/fota)
target_include_directories(app PRIVATE ../../../app/src/modules/location)
target_include_directories(app PRIVATE ../../../app/src/common)
target_include_directories(app PRIVATE ../../../app/src/modules/network)

zephyr_include_directories(src)
zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/net)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(${NRF_DIR}/include/modem)
zephyr_include_directories(${NRF_DIR}/include/net)
zephyr_include_directories(${NRF_DIR}/include/hw_id)
zephyr_include_directories(${NRFXLIB_DIR}/nrf_modem/include)
zephyr_include_directories(${NRF_DIR}/include/net/mqtt_helper)

# Options that cannot be passed through Kconfig fragments
target_compile_definitions(app PRIVATE
	-DCONFIG_APP_CLOUD_MQTT_PAYLOAD_BUFFER_MAX_SIZE=256
	-DCONFIG_APP_CLOUD_MQTT_SHADOW_RESPONSE_BUFFER_MAX_SIZE=256
	-DCONFIG_APP_CLOUD_MQTT_LOG_LEVEL=4
	-DCONFIG_APP_CLOUD_MQTT_SEC_TAG=42
	-DCONFIG_APP_CLOUD_MQTT_HOSTNAME="test-mqtt.org"
	-DCONFIG_APP_CLOUD_MQTT_PUB_TOPIC="data"
	-DCONFIG_APP_CLOUD_MQTT_SUB_TOPIC="commands"
	-DCONFIG_APP_CLOUD_MQTT_TOPIC_SIZE_MAX=128
	-DCONFIG_APP_CLOUD_MQTT_THREAD_STACK_SIZE=2048
	-DCONFIG_APP_CLOUD_MQTT_MESSAGE_QUEUE_SIZE=5
	-DCONFIG_APP_CLOUD_MQTT_MSG_PROCESSING_TIMEOUT_SECONDS=1
	-DCONFIG_APP_CLOUD_MQTT_WATCHDOG_TIMEOUT_SECONDS=2
	-DCONFIG_APP_CLOUD_MQTT_BACKOFF_TYPE_LINEAR=1
	-DCONFIG_APP_CLOUD_MQTT_BACKOFF_INITIAL_SECONDS=10
	-DCONFIG_APP_CLOUD_MQTT_BACKOFF_LINEAR_INCREMENT_SECONDS=60
	-DCONFIG_APP_CLOUD_MQTT_BACKOFF_MAX_SECONDS=3600
	-DCONFIG_HW_ID_LIBRARY_SOURCE_IMEI=1
	-DATT_MQTT_CA_CERT="ca-cert.inc"
	-DCONFIG_LOCATION_SERVICE_EXTERNAL=1
	-DCONFIG_LOCATION_METHOD_CELLULAR=1
	-DCONFIG_LOCATION_METHOD_WIFI=1
	-DCONFIG_LOCATION_METHODS_LIST_SIZE=3
	-DCONFIG_NRF_CLOUD_AGNSS=y
)
