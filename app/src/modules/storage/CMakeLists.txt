#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_include_directories(app PRIVATE .)

target_sources(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/storage.c
	${CMAKE_CURRENT_SOURCE_DIR}/storage_data_types.c
)

zephyr_linker_sources(SECTIONS storage_sections.ld)

if(CONFIG_APP_STORAGE_BACKEND_RAM)
	target_sources(app PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/backends/ram_backend.c
	)
endif()

if(CONFIG_APP_STORAGE_BACKEND_INTERNAL_FLASH)
	target_sources(app PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/backends/flash_backend.c
	)
endif()
