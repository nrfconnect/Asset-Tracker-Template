##########################################################################################
# Copyright (c) 2025 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import os
import pytest
from utils.flash_tools import flash_device, reset_device
import sys
sys.path.append(os.getcwd())
from utils.logger import get_logger

logger = get_logger()

def test_gnss(dut_board, hex_file):
    '''
    Test that the device gets a GNSS fix and reports its location.

    Check that the reported location is within a reasonable range of the expected location.
    '''

    flash_device(os.path.abspath(hex_file))
    dut_board.uart.xfactoryreset()

    dut_board.uart.wait_for_str_with_retries(
        "location_module: location_event_handler: Got location:",
        max_retries=5,
        timeout=120,
        reset_func=reset_device)

    res = dut_board.uart.extract_value(
        r"location_module: location_event_handler: Got location: lat: (\d+\.\d+), lon: (\d+\.\d+), acc: (\d+\.\d+), method: ([a-zA-Z]+)"
    )
    assert res, "Failed to extract location data from UART output"
    print(res)
    lat, lon, acc, method = res
    try:
        lat = float(lat)
        lon = float(lon)
        acc = float(acc)
    except ValueError as e:
        pytest.fail(f"Failed to convert location data to float: {e}")
    # Node located at approx 61.493219, 23.771307
    assert abs(lat - 61.493219) < 0.1, f"Latitude {lat} is not within expected range of 61.493219"
    assert abs(lon - 23.771307) < 0.1, f"Longitude {lon} is not within expected range of 23.771307"
    assert acc > 0, f"Accuracy {acc} should be greater than 0"
    assert "GNSS" in method, f"Method '{method}' should be 'GNSS'"

    logger.info(f"Got location: lat: {lat}, lon: {lon}, acc: {acc}, method: {method}")
