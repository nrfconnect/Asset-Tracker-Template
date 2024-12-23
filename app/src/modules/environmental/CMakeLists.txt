#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_sources_ifdef(CONFIG_APP_ENVIRONMENTAL app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/environmental.c
)
