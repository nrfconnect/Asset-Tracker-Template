#
# Copyright (c) 2025-2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

# SB_CONFIG_LAUNCH_PPR: add the application core side of the PPR log forwarding
# (IPC log link + PPR launch) to the default image. The PPR side lives in the
# ppr image's own prj.conf/app.overlay.
#
# A defined <image>_EXTRA_CONF_FILE / <image>_EXTRA_DTC_OVERLAY_FILE replaces the
# unprefixed -DEXTRA_CONF_FILE / -DEXTRA_DTC_OVERLAY_FILE for the main image
# instead of merging with it, so carry the unprefixed value over first.
if(SB_CONFIG_LAUNCH_PPR)
  sysbuild_cache_set(VAR ${DEFAULT_IMAGE}_EXTRA_CONF_FILE APPEND REMOVE_DUPLICATES
                     ${EXTRA_CONF_FILE} ${APP_DIR}/ppr/cpuapp_ipc_log.conf)
  sysbuild_cache_set(VAR ${DEFAULT_IMAGE}_EXTRA_DTC_OVERLAY_FILE APPEND REMOVE_DUPLICATES
                     ${EXTRA_DTC_OVERLAY_FILE} ${APP_DIR}/ppr/cpuapp_ipc_log.overlay)
endif()

# Generate the full programmable image at build/merged.hex (b0 + signed
# mcuboot s0/s1 + signed TF-M+app + app_provision).
#
# Only meaningful when NSIB (b0) is built. Targets without a bootloader, such as
# nRF9251, produce none of these inputs, and an unconditional ALL target makes
# the build fail on the missing b0 hex.
#
# On nRF9251 (MCUboot + generated UICR, no b0) merged.hex is instead made from
# mcuboot + uicr + the signed app, in the same order as the sysbuild flash order,
# plus the PPR image when SB_CONFIG_LAUNCH_PPR is enabled.
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
  set(att_merged_images mcuboot uicr)

  if(SB_CONFIG_LAUNCH_PPR)
    list(APPEND att_merged_inputs
         ${CMAKE_BINARY_DIR}/${SB_CONFIG_PPRCORE_IMAGE_NAME}/zephyr/zephyr.hex)
    list(APPEND att_merged_images ${SB_CONFIG_PPRCORE_IMAGE_NAME})
  endif()

  add_custom_command(
    OUTPUT ${att_merged_hex}
    COMMAND ${PYTHON_EXECUTABLE}
            ${ZEPHYR_BASE}/scripts/build/mergehex.py
            -o ${att_merged_hex}
            --overlap replace
            ${att_merged_inputs}
    DEPENDS ${att_merged_inputs}
    COMMENT "Generating merged.hex (mcuboot + uicr + signed app [+ ppr])"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    VERBATIM
  )

  add_custom_target(att_merged_hex ALL DEPENDS ${att_merged_hex})
  add_dependencies(att_merged_hex ${DEFAULT_IMAGE} ${att_merged_images})
endif()
