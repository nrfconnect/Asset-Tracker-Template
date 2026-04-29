##########################################################################################
# Copyright (c) 2025 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import os
import time
from utils.flash_tools import flash_device, reset_device
import sys
sys.path.append(os.getcwd())
from utils.logger import get_logger

logger = get_logger()

CLOUD_TIMEOUT = 60 * 3
DEFAULT_SAMPLE_INTERVAL = 600
DEFAULT_STORAGE_THRESHOLD = 1
BOOT_SAMPLE_INTERVAL = 60
BOOT_STORAGE_THRESHOLD = 5
RUNTIME_SAMPLE_INTERVAL = 120
RUNTIME_STORAGE_THRESHOLD = 3


def wait_for_config_reported(cloud, device_id, expected_sample, expected_threshold):
    """Poll the cloud until the device reports the expected config values."""
    start = time.time()
    sample_interval = storage_threshold = None
    while time.time() - start < CLOUD_TIMEOUT:
        time.sleep(5)
        try:
            device = cloud.get_device(device_id)
            device_state = device["state"]
            sample_interval = device_state["reported"]["config"]["sample_interval"]
            storage_threshold = device_state["reported"]["config"]["storage_threshold"]
        except (KeyError, TypeError) as e:
            # Expected while the device has not yet reported its shadow: the
            # `reported.config.*` keys are missing. Keep polling.
            logger.debug(f"Reported config not available yet: {e}")
            continue

        logger.debug(f"Device state: {device_state}")

        if (sample_interval == expected_sample and
            storage_threshold == expected_threshold):
            return

        logger.debug(
            f"Config not yet fully updated. Current: "
            f"sample_interval={sample_interval}, "
            f"storage_threshold={storage_threshold}"
        )

    raise RuntimeError(
        f"Configuration not correctly reported to cloud. "
        f"Expected: sample_interval={expected_sample}, "
        f"storage_threshold={expected_threshold}. "
        f"Got: sample_interval={sample_interval}, "
        f"storage_threshold={storage_threshold}"
    )


def test_config(dut_cloud, hex_file):
    '''
    Test that verifies shadow changes are applied and reported back for all config
    parameters. Tests both boot-time config pickup and runtime config updates.
    '''

    # Clear shadow config and command sections before starting the test to ensure a clean slate
    # and deterministic behavior.
    dut_cloud.cloud.patch_reset_config_and_command(dut_cloud.device_id)

    # Phase 1: Boot-time config
    # Clear and patch config before booting so the device picks it up from the shadow on first connect.
    dut_cloud.cloud.patch_config(
        dut_cloud.device_id,
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
            BOOT_SAMPLE_INTERVAL, BOOT_STORAGE_THRESHOLD
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
            sample_interval=RUNTIME_SAMPLE_INTERVAL,
            storage_threshold=RUNTIME_STORAGE_THRESHOLD
        )

        # Trigger a shadow delta poll via shell command so the device picks up the new config
        dut_cloud.uart.write("att_cloud poll_shadow_delta\r\n")

        wait_for_config_reported(
            dut_cloud.cloud, dut_cloud.device_id,
            RUNTIME_SAMPLE_INTERVAL, RUNTIME_STORAGE_THRESHOLD
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
            sample_interval=DEFAULT_SAMPLE_INTERVAL,
            storage_threshold=DEFAULT_STORAGE_THRESHOLD
        )
