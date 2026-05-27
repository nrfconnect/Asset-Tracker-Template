#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

zephyr_library()
zephyr_library_sources(SGP4.c)
zephyr_library_sources_ifdef(CONFIG_SGP4_PASS_PREDICT sgp4_pass_predict.c)
zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR})
