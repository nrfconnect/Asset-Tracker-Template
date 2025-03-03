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

def test_sampling(t91x_board, hex_file):
    flash_device(os.path.abspath(hex_file))
    t91x_board.uart.xfactoryreset()
    patterns_cloud_connection = [
        "Network connectivity established",
        "Connected to Cloud"
    ]

    # Log patterns
    pattern_location = "location_event_handler: Got location: lat:"
    pattern_shadow_poll = "Requesting device shadow from the device"
    pattern_environmental = "Environmental values sample request received, getting data"
    pattern_fota_poll = "Checking for FOTA job..."
    pattern_battery = "State of charge:"

    # Combine patterns for convenience
    pattern_list = [
        pattern_location,
        pattern_shadow_poll,
        pattern_environmental,
        pattern_fota_poll,
        pattern_battery
    ]

    # Cloud connection
    t91x_board.uart.flush()
    reset_device()

    # Sampling
    t91x_board.uart.wait_for_str(pattern_list, timeout=120)

    # Extract coordinates from UART output
    values = t91x_board.uart.extract_value( \
        r"location_event_handler: Got location: lat: ([\d.]+), lon: ([\d.]+), acc: ([\d.]+), method: Wi-Fi")
    assert values

    lat, lon, acc = values
    assert abs(float(lat) - 61.5) < 2 and abs(float(lon) - 10.5) < 1
