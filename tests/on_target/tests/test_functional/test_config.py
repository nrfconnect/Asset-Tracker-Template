##########################################################################################
# Copyright (c) 2025 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import os
import pytest
import time
from utils.flash_tools import flash_device, reset_device
import sys
sys.path.append(os.getcwd())
from utils.logger import get_logger

logger = get_logger()

CLOUD_TIMEOUT = 60 * 3
DEFAULT_UPDATE_INTERVAL = 600
TEST_UPDATE_INTERVAL = 150

def test_config(dut_cloud, hex_file):
    '''
    Test that verifies shadow changes are applied c2d and d2c.
    '''
    flash_device(os.path.abspath(hex_file))
    dut_cloud.uart.xfactoryreset()
    dut_cloud.uart.flush()
    reset_device()

    dut_cloud.uart.wait_for_str_with_retries("Connected to Cloud", max_retries=3, timeout=240, reset_func=reset_device)

    dut_cloud.cloud.patch_update_interval(dut_cloud.device_id, interval=TEST_UPDATE_INTERVAL)

    # # Wait for shadow to be reported to cloud
    start = time.time()
    try:
        while time.time() - start < CLOUD_TIMEOUT:
            time.sleep(5)
            try:
                device = dut_cloud.cloud.get_device(dut_cloud.device_id)
                device_state = device["state"]
                update_interval = device_state["reported"]["config"]["update_interval"]
            except Exception as e:
                pytest.skip(f"Unable to retrieve device state from cloud, e: {e}")

            logger.debug(f"Device state: {device_state}")
            if update_interval == TEST_UPDATE_INTERVAL:
                break
            else:
                logger.debug("No correct interval update reported yet, retrying...")
                continue
        else:
            raise RuntimeError(f"No correct update interval reported back to cloud, desired {TEST_UPDATE_INTERVAL}, reported {update_interval}")
        # Wait for update interval to be reported in the device log
        dut_cloud.uart.wait_for_str(f"main: handle_cloud_shadow_response: Received new interval: {TEST_UPDATE_INTERVAL} seconds", timeout=120)
    finally:
        # Back to default interval no matter what
        dut_cloud.cloud.patch_update_interval(dut_cloud.device_id, interval=DEFAULT_UPDATE_INTERVAL)
