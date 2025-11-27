#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cloud.c)
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cloud_provisioning.c)
target_sources_ifdef(CONFIG_APP_ENVIRONMENTAL app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cloud_environmental.c)
target_sources_ifdef(CONFIG_APP_CLOUD_SHELL app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cloud_shell.c)
target_include_directories(app PRIVATE .)

if (CONFIG_NRF_CLOUD_COAP_SEC_TAG GREATER_EQUAL 2147483648 AND CONFIG_NRF_CLOUD_COAP_SEC_TAG LESS_EQUAL 2147483667)
	message(WARNING "CONFIG_NRF_CLOUD_COAP_SEC_TAG is set to a developer security tag. "
			"TLS traffic can now be decrypted with Nordic tools. "
			"This should only be used during development and testing.")
endif()
