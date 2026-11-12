#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(fota_components_test)

test_runner_generate(src/fota_components_test.c)

set(ASSET_TRACKER_TEMPLATE_DIR ../../..)
set(MEMFAULT_NCS_PORT ${ZEPHYR_BASE}/../modules/lib/memfault-firmware-sdk/ports/zephyr/ncs)

target_sources(app
	PRIVATE
	src/fota_components_test.c
	${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/fota/fota_components.c
)

# The registry of OTA components is an iterable section, linked as in the Memfault port
zephyr_linker_sources(SECTIONS ${MEMFAULT_NCS_PORT}/memfault-fota-component.ld)
zephyr_iterable_section(NAME MemfaultFotaComponent KVMA RAM_REGION GROUP RODATA_REGION
			SUBALIGN 4)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)

zephyr_include_directories(${MEMFAULT_NCS_PORT}/include)
zephyr_include_directories(${NRF_DIR}/include)
zephyr_include_directories(${NRF_DIR}/subsys/net/lib/nrf_cloud/common/include)

target_link_options(app PRIVATE --whole-archive)

# Options that cannot be passed through Kconfig fragments
target_compile_definitions(app PRIVATE
	-DCONFIG_DOWNLOADER_MAX_HOSTNAME_SIZE=128
	-DCONFIG_DOWNLOADER_MAX_FILENAME_SIZE=256
	-DCONFIG_DOWNLOADER_TRANSPORT_PARAMS_SIZE=64
	-DCONFIG_DOWNLOADER_STACK_SIZE=4096
)
