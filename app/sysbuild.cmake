#
# Copyright (c) 2025-2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

# Generate the full programmable image at build/merged.hex (b0 + signed
# mcuboot s0/s1 + signed TF-M+app + app_provision).
#
# Only meaningful when NSIB (b0) is built. Targets without a bootloader, such as
# nRF9251, produce none of these inputs, and an unconditional ALL target makes
# the build fail on the missing b0 hex.
#
# On nRF9251 (MCUboot + generated UICR, no b0) merged.hex is instead made from
# mcuboot + uicr + the signed app, in the same order as the sysbuild flash order.
if(SB_CONFIG_SECURE_BOOT_APPCORE)
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
elseif(SB_CONFIG_BOOTLOADER_MCUBOOT AND SB_CONFIG_NRF_GENERATE_UICR)
  set(att_merged_hex ${CMAKE_BINARY_DIR}/merged.hex)
  set(att_merged_inputs
      ${CMAKE_BINARY_DIR}/mcuboot/zephyr/zephyr.hex
      ${CMAKE_BINARY_DIR}/uicr/zephyr/zephyr.hex
      ${CMAKE_BINARY_DIR}/app/zephyr/zephyr.signed.hex
  )

  add_custom_command(
    OUTPUT ${att_merged_hex}
    COMMAND ${PYTHON_EXECUTABLE}
            ${ZEPHYR_BASE}/scripts/build/mergehex.py
            -o ${att_merged_hex}
            --overlap replace
            ${att_merged_inputs}
    DEPENDS ${att_merged_inputs}
    COMMENT "Generating merged.hex (mcuboot + uicr + signed app)"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    VERBATIM
  )

  add_custom_target(att_merged_hex ALL DEPENDS ${att_merged_hex})
  add_dependencies(att_merged_hex ${DEFAULT_IMAGE} mcuboot uicr)
endif()
