#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_sources(app PRIVATE
    sat_prediction.c
    SGP4.c
)

target_include_directories(app PRIVATE .)
