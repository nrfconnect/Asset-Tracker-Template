#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inspect_shell.c)
zephyr_linker_sources(SECTIONS inspect_sections.ld)
target_include_directories(app PRIVATE .)
