#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sgp4_pass_predict.c)
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/SGP4.c)

target_include_directories(app PRIVATE .)
