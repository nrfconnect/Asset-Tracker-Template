# Copyright (c) 2024 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(gas_sensor.c)
zephyr_library_include_directories("${ZEPHYR_NRF_MODULE_DIR}/include/drivers/")
zephyr_include_directories(.)
