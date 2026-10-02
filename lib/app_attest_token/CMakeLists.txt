#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

zephyr_library()

zephyr_library_sources(
	app_attest_token.c
)

zephyr_link_libraries_ifdef(CONFIG_TF_PSA_CRYPTO_BUILTIN tfpsacrypto)
