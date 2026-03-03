#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

# Override the board-level static partition layout with an app-specific one.
# This avoids modifying the SDK and keeps partition changes in the project.
if(SB_CONFIG_BOARD_THINGY91X_NRF9151_NS)
  set(PM_STATIC_YML_FILE
    ${CMAKE_CURRENT_SOURCE_DIR}/pm_static_thingy91x_nrf9151_ns.yml
    CACHE INTERNAL "" FORCE
  )
endif()
