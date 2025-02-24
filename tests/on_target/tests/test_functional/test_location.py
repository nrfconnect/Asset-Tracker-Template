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

def test_wifi_location(t91x_board, hex_file):
    '''
    Test that the device can get location using Wi-Fi
    '''
    run_location(t91x_board, hex_file, location_method="Wi-Fi")

@pytest.mark.skip(reason="GNSS location is not supported on this device")
def test_gnss_location(t91x_board, hex_file):
    '''
    Test that the device can get location using GNSS
    '''
    run_location(t91x_board, hex_file, location_method="GNSS")

def run_location(t91x_board, hex_file, location_method):
    flash_device(os.path.abspath(hex_file))
    t91x_board.uart.xfactoryreset()
    patterns_cloud_connection = [
        "Network connectivity established",
        "Connected to Cloud"
    ]

    patterns_location = ["Wi-Fi and cellular methods combined"] if location_method == "Wi-Fi" else []
    patterns_location = patterns_location + [
        "location_event_handler: Got location: lat:"]

    # Cloud connection
    t91x_board.uart.flush()
    reset_device()
    t91x_board.uart.wait_for_str(patterns_cloud_connection, timeout=120)

    # Location
    t91x_board.uart.wait_for_str(patterns_location, timeout=180)

    # Extract coordinates from UART output
    values = t91x_board.uart.extract_value( \
        r"location_event_handler: Got location: lat: ([\d.]+), lon: ([\d.]+), acc: ([\d.]+), method: ([\d.]+)")
    assert values
    lat, lon, acc, method = values
    assert abs(float(lat) - 61.5) < 2 and abs(float(lon) - 10.5) < 1
    method = int(method)
    expected_method = 4 if location_method == "Wi-Fi" else 1
    assert method == expected_method
