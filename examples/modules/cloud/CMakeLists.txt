#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

# Prevent this example module from being included in images that doesn't enable the MQTT cloud module
if (NOT CONFIG_APP_CLOUD_MQTT)
	return()
endif()

add_subdirectory(creds)

zephyr_sources(${CMAKE_CURRENT_SOURCE_DIR}/cloud_mqtt.c)
zephyr_sources(${CMAKE_CURRENT_SOURCE_DIR}/cloud_mqtt_shell.c)

if (CONFIG_NRF_CLOUD_COAP_SEC_TAG GREATER_EQUAL 2147483648 AND CONFIG_NRF_CLOUD_COAP_SEC_TAG LESS_EQUAL 2147483667)
	message(WARNING "CONFIG_NRF_CLOUD_COAP_SEC_TAG is set to a developer security tag. "
			"TLS traffic can now be decrypted with Nordic tools. "
			"This should only be used during development and testing.")
endif()
