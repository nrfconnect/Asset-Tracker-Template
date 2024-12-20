##########################################################################################
# Copyright (c) 2024 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import pytest
import time
import os
import re
from utils.flash_tools import flash_device, reset_device
import sys
sys.path.append(os.getcwd())
from utils.logger import get_logger

logger = get_logger()

@pytest.mark.dut1
@pytest.mark.uart
def test_uart_output(t91x_board, hex_file):
    flash_device(os.path.abspath(hex_file))
    t91x_board.uart.xfactoryreset()
    patterns_boot = [
            "Network connectivity established",
            "Connected to Cloud",
            "trigger: frequent_poll_entry: frequent_poll_entry",
            "trigger: trigger_work_fn: Sending data sample trigger",
            "environmental_module: sample: temp:",
            "transport: state_connected_ready_run: Payload",
            "Location search done"
    ]
    patterns_button_press = [
        "trigger: frequent_poll_run: Button 1 pressed in frequent poll state, restarting duration timer",
        "trigger_poll_work_fn: Sending shadow/fota poll trigger"
    ]
    patterns_lte_offline = [
            "network: Network connectivity lost",
    ]
    patterns_lte_normal = [
            "network: Network connectivity established",
    ]

    # Boot
    t91x_board.uart.flush()
    reset_device()
    t91x_board.uart.wait_for_str(patterns_boot, timeout=120)

    # Button press
    t91x_board.uart.flush()
    t91x_board.uart.write("zbus button_press\r\n")
    t91x_board.uart.wait_for_str(patterns_button_press, timeout=120)

    # LTE disconnect
    t91x_board.uart.flush()
    t91x_board.uart.write("lte offline\r\n")
    t91x_board.uart.wait_for_str(patterns_lte_offline, timeout=20)

    # LTE reconnect
    t91x_board.uart.flush()
    t91x_board.uart.write("lte normal\r\n")
    t91x_board.uart.wait_for_str(patterns_lte_normal, timeout=120)
