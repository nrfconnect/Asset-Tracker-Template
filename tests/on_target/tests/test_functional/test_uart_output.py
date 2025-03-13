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

def test_uart_output(dut_board, hex_file):
    '''
    Test that the device is operating normally by checking UART output
    '''
    flash_device(os.path.abspath(hex_file))
    dut_board.uart.xfactoryreset()
    patterns_boot = [
        "Network connectivity established",
        "Connected to Cloud",
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
    dut_board.uart.flush()
    reset_device()
    dut_board.uart.wait_for_str(patterns_boot, timeout=120)

    # # Button press
    # dut_board.uart.flush()
    # dut_board.uart.write("zbus button_press\r\n")
    # dut_board.uart.wait_for_str(patterns_button_press, timeout=120)

    # # LTE disconnect
    # dut_board.uart.flush()
    # dut_board.uart.write("lte offline\r\n")
    # dut_board.uart.wait_for_str(patterns_lte_offline, timeout=20)

    # # LTE reconnect
    # dut_board.uart.flush()
    # dut_board.uart.write("lte normal\r\n")
    # dut_board.uart.wait_for_str(patterns_lte_normal, timeout=120)
