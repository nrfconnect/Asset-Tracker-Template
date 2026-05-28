##########################################################################################
# Copyright (c) 2025 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import os
import sys
import pytest
from time import sleep
from utils.flash_tools import flash_device, reset_device

sys.path.append(os.getcwd())
from utils.logger import get_logger

logger = get_logger()

DEFAULT_SAMPLE_INTERVAL = 150
DEFAULT_STORAGE_THRESHOLD = 1
FLASH_BUFFER_TEST_SAMPLE_INTERVAL = 15
FLASH_BUFFER_TEST_STORAGE_THRESHOLD = 10

def get_storing_str(datatype, file_index=0):
    return "Storing data in file /att_storage/" + datatype + "_" + str(file_index) + ".bin"

def get_init_header_str(datatype):
    return "Initialized header file /att_storage/" + datatype + ".header"

def get_open_header_str(datatype):
    return "Opened header file /att_storage/" + datatype + ".header"

@pytest.mark.slow
def test_buffer_flash(dut_cloud, hex_file_buffer_flash):

    # Clear shadow config and command sections before starting the test to ensure a clean slate
    # and deterministic behavior.
    dut_cloud.cloud.patch_reset_config_and_command(dut_cloud.device_id)

    # Change cloud config to enable buffer mode and set short sampling interval
    dut_cloud.cloud.patch_config(
        dut_cloud.device_id,
        sample_interval=FLASH_BUFFER_TEST_SAMPLE_INTERVAL,
        storage_threshold=FLASH_BUFFER_TEST_STORAGE_THRESHOLD
    )

    flash_device(os.path.abspath(hex_file_buffer_flash))
    dut_cloud.uart.xfactoryreset()

    clear_str = "att_storage clear\r\n"

    storing_list = [
        get_storing_str("LOCATION"),
        get_storing_str("BATTERY"),
        get_storing_str("ENVIRONMENTAL")
    ]

    init_header_list = [
        get_init_header_str("LOCATION"),
        get_init_header_str("BATTERY"),
        get_init_header_str("ENVIRONMENTAL")
    ]

    open_header_list = [
        get_open_header_str("LOCATION"),
        get_open_header_str("BATTERY"),
        get_open_header_str("ENVIRONMENTAL")
    ]

    try:
        reset_device()
        start_pos = dut_cloud.uart.get_size()

        # Wait for storage automount
        dut_cloud.uart.wait_for_str("Automount /att_storage succeeded", timeout=60, start_pos=start_pos)

        # Clear buffer
        start_pos = dut_cloud.uart.get_size()
        dut_cloud.uart.write(clear_str)

        # Header files initialized
        dut_cloud.uart.wait_for_str(init_header_list, timeout=60, start_pos=start_pos)

        # Initial data storing
        dut_cloud.uart.wait_for_str(storing_list, timeout=60, start_pos=start_pos)

        # Location file rollover, expected on sample 9
        start_pos = dut_cloud.uart.get_size()
        dut_cloud.uart.wait_for_str(get_storing_str("LOCATION", file_index=1), timeout=300, start_pos=start_pos)

        # Wait for buffer processing and pipe exit using the same start_pos captured before the rollover.
        # Avoids a race where "All items consumed" or "state_buffer_pipe_active_exit" is logged between
        # the rollover detection and a new get_size() call, which would cause the search to miss them.
        dut_cloud.uart.wait_for_str(
            "All items consumed, pipe empty",
            timeout=300,
            start_pos=start_pos,
        )

        dut_cloud.uart.wait_for_str("state_buffer_pipe_active_exit", timeout=120, start_pos=start_pos)

        # Capture write and read offsets before reboot (only using LOCATION as all types should be in sync)
        start_pos = dut_cloud.uart.get_size()
        dut_cloud.uart.write("at AT\r\n")
        pre_reboot_offsets = []
        try:
            offsets = dut_cloud.uart.wait_for_str_re(
                r"Storing data in file /att_storage/ENVIRONMENTAL.*write_offset=(\d+), read_offset=(\d+)",
                timeout=120,
                start_pos=start_pos,
            )
            if offsets:
                write_offset = int(offsets[0])
                read_offset = int(offsets[1])
                pre_reboot_offsets = [write_offset, read_offset]
                logger.info(f"Pre-reboot offsets: write_offset={pre_reboot_offsets[0]}, read_offset={pre_reboot_offsets[1]}")
            else:
                pytest.fail("Failed to extract pre-reboot offsets")
        except Exception as e:
            logger.warning(f"Exception while extracting pre-reboot offsets: {e}")
            dut_cloud.uart.write("at AT\r\n")
            sleep(20)

        # Reboot device
        reset_device()
        reboot_start_pos = dut_cloud.uart.get_size()

        # Header files re-opened from existing data after reboot
        dut_cloud.uart.wait_for_str(open_header_list, timeout=120, start_pos=reboot_start_pos)

        # Capture write and read offsets after reboot (only using LOCATION as all types should be in sync)
        post_reboot_offsets = []
        offsets = dut_cloud.uart.wait_for_str_re(
            r"Storing data in file /att_storage/ENVIRONMENTAL.*write_offset=(\d+), read_offset=(\d+)",
            timeout=120,
            start_pos=reboot_start_pos,
        )
        if offsets:
            write_offset = int(offsets[0])
            read_offset = int(offsets[1])
            post_reboot_offsets = [write_offset, read_offset]
            logger.info(f"Post-reboot offsets: write_offset={post_reboot_offsets[0]}, read_offset={post_reboot_offsets[1]}")
        else:
            pytest.fail("Failed to extract post-reboot offsets")

        # Assert offsets, write_offset should have increased by one, read_offset should be unchanged
        assert post_reboot_offsets[0] == pre_reboot_offsets[0] + 1, "write_offset did not increase by one after reboot"
        assert post_reboot_offsets[1] == pre_reboot_offsets[1], "read_offset changed after reboot"
    finally:
        # Restore default config no matter what
        dut_cloud.cloud.patch_config(
            dut_cloud.device_id,
            sample_interval=DEFAULT_SAMPLE_INTERVAL,
            storage_threshold=DEFAULT_STORAGE_THRESHOLD
        )
