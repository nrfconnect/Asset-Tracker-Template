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
DEFAULT_UPDATE_INTERVAL = 3600
DEFAULT_SAMPLE_INTERVAL = 600
DEFAULT_STORAGE_THRESHOLD = 1
BOOT_UPDATE_INTERVAL = 300
BOOT_SAMPLE_INTERVAL = 60
BOOT_STORAGE_THRESHOLD = 5
RUNTIME_UPDATE_INTERVAL = 1800
RUNTIME_SAMPLE_INTERVAL = 120
RUNTIME_STORAGE_THRESHOLD = 3


def wait_for_config_reported(cloud, device_id, expected_update, expected_sample, expected_threshold):
    """Poll the cloud until the device reports the expected config values."""
    start = time.time()
    while time.time() - start < CLOUD_TIMEOUT:
        time.sleep(5)
        try:
            device = cloud.get_device(device_id)
            device_state = device["state"]
            update_interval = device_state["reported"]["config"]["update_interval"]
            sample_interval = device_state["reported"]["config"]["sample_interval"]
            storage_threshold = device_state["reported"]["config"]["storage_threshold"]
        except Exception as e:
            pytest.skip(f"Unable to retrieve device state from cloud, e: {e}")

        logger.debug(f"Device state: {device_state}")

        if (update_interval == expected_update and
            sample_interval == expected_sample and
            storage_threshold == expected_threshold):
            return
        else:
            logger.debug(
                f"Config not yet fully updated. Current: "
                f"update_interval={update_interval}, "
                f"sample_interval={sample_interval}, "
                f"storage_threshold={storage_threshold}"
            )
            continue

    raise RuntimeError(
        f"Configuration not correctly reported to cloud. "
        f"Expected: update_interval={expected_update}, "
        f"sample_interval={expected_sample}, "
        f"storage_threshold={expected_threshold}. "
        f"Got: update_interval={update_interval}, "
        f"sample_interval={sample_interval}, "
        f"storage_threshold={storage_threshold}"
    )


def test_config(dut_cloud, hex_file):
    '''
    Test that verifies shadow changes are applied and reported back for all config
    parameters. Tests both boot-time config pickup and runtime config updates.
    '''

    # Phase 1: Boot-time config
    # Patch config before booting so the device picks it up from the shadow on first connect.
    dut_cloud.cloud.patch_config(
        dut_cloud.device_id,
        update_interval=BOOT_UPDATE_INTERVAL,
        sample_interval=BOOT_SAMPLE_INTERVAL,
        storage_threshold=BOOT_STORAGE_THRESHOLD
    )

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

    try:
        wait_for_config_reported(
            dut_cloud.cloud, dut_cloud.device_id,
            BOOT_UPDATE_INTERVAL, BOOT_SAMPLE_INTERVAL, BOOT_STORAGE_THRESHOLD
        )

        dut_cloud.uart.wait_for_str(
            f"Updating update interval to {BOOT_UPDATE_INTERVAL} seconds",
            timeout=120
        )
        dut_cloud.uart.wait_for_str(
            f"Updating sample interval to {BOOT_SAMPLE_INTERVAL} seconds",
            timeout=120
        )
        dut_cloud.uart.wait_for_str(
            f"storage: update_threshold: Updating buffer threshold limit: {BOOT_STORAGE_THRESHOLD}",
            timeout=120
        )

        # Phase 2: Runtime config update
        # Patch new config values while the device is already running and connected.
        dut_cloud.uart.flush()
        dut_cloud.cloud.patch_config(
            dut_cloud.device_id,
            update_interval=RUNTIME_UPDATE_INTERVAL,
            sample_interval=RUNTIME_SAMPLE_INTERVAL,
            storage_threshold=RUNTIME_STORAGE_THRESHOLD
        )

        # Trigger a shadow delta poll via shell command so the device picks up the new config
        dut_cloud.uart.write("att_cloud poll_shadow_delta\r\n")

        wait_for_config_reported(
            dut_cloud.cloud, dut_cloud.device_id,
            RUNTIME_UPDATE_INTERVAL, RUNTIME_SAMPLE_INTERVAL, RUNTIME_STORAGE_THRESHOLD
        )

        dut_cloud.uart.wait_for_str(
            f"Updating update interval to {RUNTIME_UPDATE_INTERVAL} seconds",
            timeout=120
        )
        dut_cloud.uart.wait_for_str(
            f"Updating sample interval to {RUNTIME_SAMPLE_INTERVAL} seconds",
            timeout=120
        )
        dut_cloud.uart.wait_for_str(
            f"storage: update_threshold: Updating buffer threshold limit: {RUNTIME_STORAGE_THRESHOLD}",
            timeout=120
        )
    finally:
        # Restore default config no matter what
        dut_cloud.cloud.patch_config(
            dut_cloud.device_id,
            update_interval=DEFAULT_UPDATE_INTERVAL,
            sample_interval=DEFAULT_SAMPLE_INTERVAL,
            storage_threshold=DEFAULT_STORAGE_THRESHOLD
        )
