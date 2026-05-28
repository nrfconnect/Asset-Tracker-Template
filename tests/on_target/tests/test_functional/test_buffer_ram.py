##########################################################################################
# Copyright (c) 2025 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import os
import sys
import pytest
from utils.flash_tools import flash_device, reset_device

sys.path.append(os.getcwd())
from utils.logger import get_logger

logger = get_logger()

DEFAULT_SAMPLE_INTERVAL = 150
DEFAULT_STORAGE_THRESHOLD = 1
RAM_BUFFER_TEST_SAMPLE_INTERVAL = 15
RAM_BUFFER_TEST_STORAGE_THRESHOLD = 10


def get_initialized_str(datatype):
    return f"Ring buffer {datatype} initialized with size"

def get_storing_str(datatype, count=None):
    if count is None:
        return f"Stored {datatype} item, count:"
    else:
        return f"Stored {datatype} item, count: {count}"

@pytest.mark.slow
def test_buffer_ram(dut_cloud, hex_file_buffer_ram):

    # Clear shadow config and command sections before starting the test to ensure a clean slate
    # and deterministic behavior.
    dut_cloud.cloud.patch_reset_config_and_command(dut_cloud.device_id)

    # Change cloud config to enable buffer mode and set short sampling interval
    dut_cloud.cloud.patch_config(
        dut_cloud.device_id,
        sample_interval=RAM_BUFFER_TEST_SAMPLE_INTERVAL,
        storage_threshold=RAM_BUFFER_TEST_STORAGE_THRESHOLD
    )


    try:
        flash_device(os.path.abspath(hex_file_buffer_ram))
        dut_cloud.uart.xfactoryreset()

        initialization_list = [
                get_initialized_str("BATTERY"),
                get_initialized_str("ENVIRONMENTAL"),
                get_initialized_str("LOCATION")
        ]

        first_storing_list = [
                get_storing_str("ENVIRONMENTAL", 1),
                get_storing_str("BATTERY", 1),
                get_storing_str("LOCATION", 1)
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
        dut_cloud.uart.wait_for_str(first_storing_list, timeout=60)

        # Wait for buffer processing, expecting all 30 items to be consumed ((BATTERY, ENVIRONMENTAL, LOCATION) x 10 samples)
        dut_cloud.uart.wait_for_str("All items consumed, pipe empty", timeout=500)

        # Wait for buffer processing to complete
        pipe_exit_pos = dut_cloud.uart.wait_for_str("state_buffer_pipe_active_exit", timeout=60)

        # Wait for next sample after buffer processing, verifying the device continues storing new items
        dut_cloud.uart.wait_for_str(storing_list, timeout=120, start_pos=pipe_exit_pos)
    finally:
        # Restore default config
        dut_cloud.cloud.patch_config(
            dut_cloud.device_id,
            sample_interval=DEFAULT_SAMPLE_INTERVAL,
            storage_threshold=DEFAULT_STORAGE_THRESHOLD
        )
