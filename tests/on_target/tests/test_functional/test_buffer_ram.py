##########################################################################################
# Copyright (c) 2025 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import os
import sys
import re
import pytest
from utils.flash_tools import flash_device, reset_device

sys.path.append(os.getcwd())
from utils.logger import get_logger

logger = get_logger()

DEFAULT_UPDATE_INTERVAL = 600
DEFAULT_SAMPLE_INTERVAL = 150
DEFAULT_BUFFER_MODE = False
RAM_BUFFER_TEST_UPDATE_INTERVAL = 180
RAM_BUFFER_TEST_SAMPLE_INTERVAL = 15
RAM_BUFFER_TEST_SAMPLE_BUFFER_MODE = True


def get_initialized_str(datatype):
    return f"Ring buffer {datatype} initialized with size"

def get_storing_str(datatype):
    return f"Stored {datatype} item, count: 1"

@pytest.mark.slow
def test_buffer_ram(dut_cloud, hex_file_buffer_ram):

    # Change cloud config to enable buffer mode and set short sampling interval
    dut_cloud.cloud.patch_config(
        dut_cloud.device_id,
        update_interval=RAM_BUFFER_TEST_UPDATE_INTERVAL,
        sample_interval=RAM_BUFFER_TEST_SAMPLE_INTERVAL,
        buffer_mode=RAM_BUFFER_TEST_SAMPLE_BUFFER_MODE
    )


    try:
        flash_device(os.path.abspath(hex_file_buffer_ram))
        dut_cloud.uart.xfactoryreset()

        clear_str = "att_storage clear\r\n"

        initialization_list = [
                get_initialized_str("BATTERY"),
                get_initialized_str("ENVIRONMENTAL"),
                get_initialized_str("LOCATION")
        ]

        storing_list = [
                get_storing_str("ENVIRONMENTAL"),
                get_storing_str("BATTERY"),
                get_storing_str("LOCATION")
        ]

        dut_cloud.uart.flush()
        reset_device()

        # Wait for storage initialization
        dut_cloud.uart.wait_for_str(initialization_list, timeout=60)

        # wait for initial storage
        dut_cloud.uart.wait_for_str(storing_list, timeout=60)

        # wait for overwrite due to full buffer (expected after 11 samples)
        dut_cloud.uart.wait_for_str("Full buffer, old data will be overwritten", timeout=300)

        # Wait for buffer processing, expecting 30 items to be stored total ((BATTERY, ENVIRONMENTAL, LOCATION) x 10 samples)
        dut_cloud.uart.wait_for_str_re(r"Batch population complete for session 0x[0-9A-F]+: \d+/30 items", timeout=120)

        # Wait for buffer processing to complete
        dut_cloud.uart.wait_for_str("state_buffer_pipe_active_exit", timeout=60)

        # Wait for next sample after buffer processing, expecting count to be back to 1
        dut_cloud.uart.flush()
        dut_cloud.uart.wait_for_str(storing_list, timeout=60)
    finally:
        # Restore default config
        dut_cloud.cloud.patch_config(
            dut_cloud.device_id,
            update_interval=DEFAULT_UPDATE_INTERVAL,
            sample_interval=DEFAULT_SAMPLE_INTERVAL,
            buffer_mode=DEFAULT_BUFFER_MODE
        )
