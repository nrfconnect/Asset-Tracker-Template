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

    # Log patterns
    pattern_location = "cloud: handle_cloud_location_request: Handling cloud location request"
    pattern_shadow_poll = "Configuration: Requesting device shadow desired from cloud"
    pattern_environmental = "Environmental values sample request received, getting data"
    pattern_fota_poll = "state_polling_for_update_entry"
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

    dut_board.uart.wait_for_str_with_retries("Connected to Cloud", max_retries=3, timeout=240, reset_func=reset_device)

    # Sampling
    dut_board.uart.wait_for_str(pattern_list, timeout=120)
