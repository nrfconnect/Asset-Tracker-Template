##########################################################################################
# Copyright (c) 2024 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import os
from utils.flash_tools import flash_device, reset_device
import sys
import pytest
from utils.flash_tools import flash_device, reset_device

sys.path.append(os.getcwd())
from utils.logger import get_logger

logger = get_logger()

def test_patched_firmware(dut_board, hex_file_patched):
    """Test the patched firmware with magnetometer functionality."""
    # Only run this test for thingy91x devices
    devicetype = os.getenv("DUT_DEVICE_TYPE")
    if devicetype != "thingy91x":
        pytest.skip("This test is only for thingy91x devices")

    # Flash the patched firmware
    flash_device(os.path.abspath(hex_file_patched))
    dut_board.uart.xfactoryreset()

    # Log patterns to check
    pattern_magnetometer = "Magnetic field data:"
    pattern_cloud = "Connected to Cloud"
    pattern_shadow = "Requesting device shadow from the device"

    # Cloud connection
    dut_board.uart.flush()
    reset_device()
    dut_board.uart.wait_for_str(pattern_cloud, timeout=120)

    # Wait for shadow request
    dut_board.uart.wait_for_str(pattern_shadow, timeout=30)

    # Wait for magnetometer readings
    dut_board.uart.wait_for_str(pattern_magnetometer, timeout=30)

    # Extract magnetometer values from UART output
    values = dut_board.uart.extract_value(
        r"Magnetic field data: X: ([\d.-]+) G, Y: ([\d.-]+) G, Z: ([\d.-]+) G"
    )
    assert values, "Failed to extract magnetometer values"

    x, y, z = map(float, values)
    # Basic validation of magnetometer values
    # These ranges are typical for magnetometer readings in microtesla
    assert -1000 <= x <= 1000, f"Magnetometer X value {x} out of expected range"
    assert -1000 <= y <= 1000, f"Magnetometer Y value {y} out of expected range"
    assert -1000 <= z <= 1000, f"Magnetometer Z value {z} out of expected range"

    dut_board.uart.write("att_dummy_request\r\n")
    dut_board.uart.wait_for_str("Dummy request sent", timeout=10)
    dut_board.uart.wait_for_str("Response received", timeout=10)
