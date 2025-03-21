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

def test_sampling(dut_board, hex_file):
    flash_device(os.path.abspath(hex_file))
    dut_board.uart.xfactoryreset()

    # Log patterns
    pattern_location = "location_event_handler: Got location: lat:"
    pattern_shadow_poll = "Requesting device shadow from the device"
    pattern_environmental = "Environmental values sample request received, getting data"
    pattern_fota_poll = "Checking for FOTA job..."
    pattern_battery = "State of charge:"

    devicetype = os.getenv("DUT_DEVICE_TYPE")

    pattern_list = [
        pattern_location,
        pattern_shadow_poll,
        pattern_fota_poll,
    ]
    if devicetype == "thingy91x":
        pattern_list.extend([pattern_battery, pattern_environmental])

    # Cloud connection
    dut_board.uart.flush()
    reset_device()
    dut_board.uart.wait_for_str("Connected to Cloud", timeout=120)

    # Sampling
    dut_board.uart.wait_for_str(pattern_list, timeout=120)

    # Extract coordinates from UART output
    values = dut_board.uart.extract_value( \
        r"location_event_handler: Got location: lat: ([\d.]+), lon: ([\d.]+), acc: ([\d.]+), method:")
    assert values

    lat, lon, acc = values
    assert abs(float(lat) - 61.5) < 2 and abs(float(lon) - 10.5) < 1
