##########################################################################################
# Copyright (c) 2025 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import os
import sys
import re
import pytest
from time import sleep
from utils.flash_tools import flash_device, reset_device

sys.path.append(os.getcwd())
from utils.logger import get_logger

logger = get_logger()

DEFAULT_UPDATE_INTERVAL = 600
DEFAULT_SAMPLE_INTERVAL = 150
DEFAULT_BUFFER_MODE = False
FLASH_BUFFER_TEST_UPDATE_INTERVAL = 180
FLASH_BUFFER_TEST_SAMPLE_INTERVAL = 15
FLASH_BUFFER_TEST_SAMPLE_BUFFER_MODE = True

def get_storing_str(datatype, file_index=0):
    return "Storing data in file /att_storage/" + datatype + "_" + str(file_index) + ".bin"

def get_header_str(datatype):
    return "Header file /att_storage/" + datatype + ".header already exists"

def get_storage_full_str(datatype):
    return "Storage full for type " + datatype + ", overwriting oldest data"

@pytest.mark.slow
def test_buffer_flash(dut_cloud, hex_file_buffer_flash):

    # Change cloud config to enable buffer mode and set short sampling interval
    dut_cloud.cloud.patch_config(
        dut_cloud.device_id,
        update_interval=FLASH_BUFFER_TEST_UPDATE_INTERVAL,
        sample_interval=FLASH_BUFFER_TEST_SAMPLE_INTERVAL,
        buffer_mode=FLASH_BUFFER_TEST_SAMPLE_BUFFER_MODE
    )

    flash_device(os.path.abspath(hex_file_buffer_flash))
    dut_cloud.uart.xfactoryreset()

    clear_str = "att_storage clear\r\n"

    storing_list = [
        get_storing_str("LOCATION"),
        get_storing_str("BATTERY"),
        get_storing_str("ENVIRONMENTAL")
    ]

    header_list = [
        get_header_str("LOCATION"),
        get_header_str("BATTERY"),
        get_header_str("ENVIRONMENTAL"),
        get_header_str("NETWORK")
    ]

    storage_full_list = [
        get_storage_full_str("LOCATION"),
        get_storage_full_str("ENVIRONMENTAL"),
        get_storage_full_str("BATTERY")
    ]

    try:
        dut_cloud.uart.flush()
        reset_device()

        # Wait for storage automount
        dut_cloud.uart.wait_for_str("/att_storage automounted", timeout=60)

        # Clear buffer
        dut_cloud.uart.write(clear_str)

        # Initial data storing
        dut_cloud.uart.wait_for_str(storing_list, timeout=60)

        # Location file rollover, expected on sample 9
        dut_cloud.uart.wait_for_str(get_storing_str("LOCATION", file_index=1), timeout=300)

        # Storage full overwriting, expected on sample 11
        dut_cloud.uart.wait_for_str(storage_full_list, timeout=100)

        # Wait for buffer processing, expecting 30 items to be stored total ((BATTERY, ENVIRONMENTAL, LOCATION) x 10 samples)
        # NOTE: If default sampling behavior changes (e.g. additional types) this needs to be updated to match expected total samples
        dut_cloud.uart.wait_for_str_re(r"Batch population complete for session 0x[0-9A-F]+: \d+/30 items", timeout=120)

        dut_cloud.uart.wait_for_str("state_buffer_pipe_active_exit", timeout=120)

        # Capture write and read offsets before reboot (only using LOCATION as all types should be in sync)
        dut_cloud.uart.flush()
        dut_cloud.uart.write("at AT\r\n")
        pre_reboot_offsets = []
        try:
            offsets = dut_cloud.uart.wait_for_str_re(get_storing_str("LOCATION") + r" .*write_offset=(\d+), read_offset=(\d+)", timeout=120)
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

        # Files exist after reboot
        dut_cloud.uart.wait_for_str(header_list, timeout=120)

        # Capture write and read offsets after reboot (only using LOCATION as all types should be in sync)
        dut_cloud.uart.flush()
        post_reboot_offsets = []
        offsets = dut_cloud.uart.wait_for_str_re(get_storing_str("LOCATION") + r" .*write_offset=(\d+), read_offset=(\d+)", timeout=120)
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
            update_interval=DEFAULT_UPDATE_INTERVAL,
            sample_interval=DEFAULT_SAMPLE_INTERVAL,
            buffer_mode=DEFAULT_BUFFER_MODE
        )
