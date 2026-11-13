#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

# Runs before sysbuild configures any image, so every image is built from the patched tree.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/apply_sdk_patches.cmake)
