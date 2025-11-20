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
DEFAULT_SAMPLE_INTERVAL = 150
DEFAULT_BUFFER_MODE = False
TEST_UPDATE_INTERVAL = 300
TEST_SAMPLE_INTERVAL = 60
TEST_BUFFER_MODE = True

def test_config(dut_cloud, hex_file):
    '''
    Test that verifies shadow changes are applied and reported back for all config
    parameters. Tests update_interval, sample_interval, and buffer_mode.
    '''
    flash_device(os.path.abspath(hex_file))
    dut_cloud.uart.xfactoryreset()
    dut_cloud.uart.flush()
    reset_device()

    dut_cloud.uart.wait_for_str_with_retries(
        "Connected to Cloud",
        max_retries=3,
        timeout=240,
        reset_func=reset_device
    )

    # Patch all config parameters to non-default values to verify config changes
    dut_cloud.cloud.patch_config(
        dut_cloud.device_id,
        update_interval=TEST_UPDATE_INTERVAL,
        sample_interval=TEST_SAMPLE_INTERVAL,
        buffer_mode=TEST_BUFFER_MODE
    )

    # Wait for shadow to be reported to cloud
    start = time.time()
    try:
        while time.time() - start < CLOUD_TIMEOUT:
            time.sleep(5)
            try:
                device = dut_cloud.cloud.get_device(dut_cloud.device_id)
                device_state = device["state"]
                update_interval = device_state["reported"]["config"][
                    "update_interval"
                ]
                sample_interval = device_state["reported"]["config"][
                    "sample_interval"
                ]
                buffer_mode = device_state["reported"]["config"]["buffer_mode"]
            except Exception as e:
                pytest.skip(
                    f"Unable to retrieve device state from cloud, e: {e}"
                )

            logger.debug(f"Device state: {device_state}")

            # Check if all config parameters have been updated
            if (update_interval == TEST_UPDATE_INTERVAL and
                sample_interval == TEST_SAMPLE_INTERVAL and
                buffer_mode == TEST_BUFFER_MODE):
                break
            else:
                logger.debug(
                    f"Config not yet fully updated. Current: "
                    f"update_interval={update_interval}, "
                    f"sample_interval={sample_interval}, "
                    f"buffer_mode={buffer_mode}"
                )
                continue
        else:
            raise RuntimeError(
                f"Configuration not correctly reported to cloud. Expected: "
                f"update_interval={TEST_UPDATE_INTERVAL}, "
                f"sample_interval={TEST_SAMPLE_INTERVAL}, "
                f"buffer_mode={TEST_BUFFER_MODE}. Got: "
                f"update_interval={update_interval}, "
                f"sample_interval={sample_interval}, "
                f"buffer_mode={buffer_mode}"
            )

        # Wait for config changes to be logged by the device
        dut_cloud.uart.wait_for_str(
            f"Updating update interval to {TEST_UPDATE_INTERVAL} seconds",
            timeout=120
        )
        dut_cloud.uart.wait_for_str(
            f"Updating sample interval to {TEST_SAMPLE_INTERVAL} seconds",
            timeout=120
        )
        dut_cloud.uart.wait_for_str("Switching to buffer mode", timeout=120)
    finally:
        # Restore default config no matter what
        dut_cloud.cloud.patch_config(
            dut_cloud.device_id,
            update_interval=DEFAULT_UPDATE_INTERVAL,
            sample_interval=DEFAULT_SAMPLE_INTERVAL,
            buffer_mode=DEFAULT_BUFFER_MODE
        )
