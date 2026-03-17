##########################################################################################
# Copyright (c) 2024 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import os
import sys
from utils.flash_tools import flash_device, reset_device

sys.path.append(os.getcwd())
from utils.logger import get_logger

logger = get_logger()

def test_sampling(dut_board, hex_file):
    flash_device(os.path.abspath(hex_file))
    dut_board.uart.xfactoryreset()

    devicetype = os.getenv("DUT_DEVICE_TYPE")

    dut_board.uart.flush()
    reset_device()

    # Sampling happens at boot while disconnected
    if devicetype == "thingy91x":
        dut_board.uart.wait_for_str(
            [
                "Environmental values sample request received, getting data",
                "State of charge:",
                "WiFi APs",
            ],
            timeout=120,
        )

    dut_board.uart.wait_for_str_with_retries(
        "Connected to Cloud", max_retries=3, timeout=240, reset_func=reset_device
    )

    # Stored data is dispatched to cloud upon connection
    patterns_after_connect = [
        "state_polling_for_update_entry",
        "Configuration: Requesting device shadow desired from cloud",
        "cloud: handle_cloud_location_request: Handling cloud location request",
    ]

    if devicetype == "thingy91x":
        patterns_after_connect.extend([
            "Battery data sent to cloud",
            "Environmental data sent to cloud",
        ])

    dut_board.uart.wait_for_str(patterns_after_connect, timeout=120)

    # Verify periodic sampling is scheduled after the first sample
    dut_board.uart.wait_for_str("Next sample trigger in", timeout=30)
