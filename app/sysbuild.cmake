#
# Copyright (c) 2025-2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

# Generate the full programmable image at build/merged.hex (b0 + signed
# mcuboot s0/s1 + signed TF-M+app + app_provision).
set(att_merged_hex ${CMAKE_BINARY_DIR}/merged.hex)
set(att_merged_inputs
    ${CMAKE_BINARY_DIR}/b0/zephyr/zephyr.hex
    ${CMAKE_BINARY_DIR}/signed_by_b0_mcuboot.hex
    ${CMAKE_BINARY_DIR}/signed_by_b0_mcuboot_s1_variant.hex
    ${CMAKE_BINARY_DIR}/app/zephyr/zephyr.signed.hex
    ${CMAKE_BINARY_DIR}/app_provision.hex
)

add_custom_command(
  OUTPUT ${att_merged_hex}
  COMMAND ${PYTHON_EXECUTABLE}
          ${ZEPHYR_BASE}/scripts/build/mergehex.py
          -o ${att_merged_hex}
          --overlap replace
          ${att_merged_inputs}
  DEPENDS ${att_merged_inputs}
  COMMENT "Generating merged.hex (b0 + mcuboot s0/s1 + signed app + app_provision)"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  VERBATIM
)

add_custom_target(att_merged_hex ALL DEPENDS ${att_merged_hex})
