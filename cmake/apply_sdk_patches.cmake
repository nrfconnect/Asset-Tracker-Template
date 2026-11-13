#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

# Applies the patches in patches/ to the west workspace before Zephyr reads its Kconfig and
# source files, so a normal build picks them up without a manual step. A patch that is already
# applied is skipped, which keeps reconfiguring an existing build directory safe.
#
# Each entry is "<file in patches/>:<west project directory, relative to the workspace topdir>".

if(DEFINED ATT_SDK_PATCHES_DONE)
  return()
endif()
set(ATT_SDK_PATCHES_DONE TRUE)

set(att_sdk_patches
  "0001-zephyr-gpio-nrfx-disable-pin-retention-nrf9251-workaround.patch:zephyr"
)

cmake_path(SET att_patch_dir NORMALIZE "${CMAKE_CURRENT_LIST_DIR}/../patches")

if(DEFINED ENV{ZEPHYR_BASE})
  cmake_path(SET att_workspace NORMALIZE "$ENV{ZEPHYR_BASE}/..")
else()
  cmake_path(SET att_workspace NORMALIZE "${CMAKE_CURRENT_LIST_DIR}/../..")
endif()

find_program(ATT_GIT_EXECUTABLE git)
if(NOT ATT_GIT_EXECUTABLE)
  message(FATAL_ERROR "git not found, cannot apply the SDK patches in ${att_patch_dir}")
endif()

foreach(entry ${att_sdk_patches})
  string(REPLACE ":" ";" entry_parts "${entry}")
  list(GET entry_parts 0 patch_file)
  list(GET entry_parts 1 project_dir)

  set(patch_path "${att_patch_dir}/${patch_file}")
  set(project_path "${att_workspace}/${project_dir}")

  if(NOT EXISTS "${patch_path}")
    message(FATAL_ERROR "SDK patch not found: ${patch_path}")
  endif()
  if(NOT IS_DIRECTORY "${project_path}")
    message(FATAL_ERROR "West project not found for patch ${patch_file}: ${project_path}")
  endif()

  # A full reverse application tells us the patch is already in the tree.
  execute_process(
    COMMAND ${ATT_GIT_EXECUTABLE} apply --reverse --check "${patch_path}"
    WORKING_DIRECTORY "${project_path}"
    RESULT_VARIABLE patch_present
    OUTPUT_QUIET
    ERROR_QUIET
  )
  if(patch_present EQUAL 0)
    message(STATUS "SDK patch already applied in ${project_dir}: ${patch_file}")
    continue()
  endif()

  execute_process(
    COMMAND ${ATT_GIT_EXECUTABLE} apply "${patch_path}"
    WORKING_DIRECTORY "${project_path}"
    RESULT_VARIABLE patch_result
    ERROR_VARIABLE patch_error
  )
  if(NOT patch_result EQUAL 0)
    message(FATAL_ERROR
      "Failed to apply ${patch_file} in ${project_path}:\n${patch_error}"
    )
  endif()

  message(STATUS "Applied SDK patch in ${project_dir}: ${patch_file}")
endforeach()
