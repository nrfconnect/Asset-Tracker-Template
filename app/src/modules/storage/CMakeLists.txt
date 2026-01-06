#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_include_directories(app PRIVATE .)

target_sources(app PRIVATE
    storage.c
    storage_data_types.c
)

zephyr_linker_sources(SECTIONS storage_sections.ld)

target_sources_ifdef(CONFIG_APP_STORAGE_BACKEND_RAM app PRIVATE
    backends/ram_ring_buffer_backend.c
)

target_sources_ifdef(CONFIG_APP_STORAGE_BACKEND_LITTLEFS app PRIVATE
    backends/littlefs_backend.c
)

target_sources_ifdef(CONFIG_APP_STORAGE_SHELL app PRIVATE
    storage_shell.c
)

target_include_directories(app PRIVATE .)
