#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_sources(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/location.c
	${CMAKE_CURRENT_SOURCE_DIR}/location_helper.c
)
target_include_directories(app PRIVATE .)
