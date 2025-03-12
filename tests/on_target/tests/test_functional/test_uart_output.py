##########################################################################################
# Copyright (c) 2024 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import os
from utils.flash_tools import flash_device, reset_device
import sys
sys.path.append(os.getcwd())
from utils.logger import get_logger

logger = get_logger()

def test_network_reconnect(dut_board, hex_file):
    '''
    Test that the device is operating normally by checking UART output
    '''
    flash_device(os.path.abspath(hex_file))
    dut_board.uart.xfactoryreset()
    patterns_boot = [
        "Network connectivity established",
        "Connected to Cloud",
    ]
    patterns_network_disconnected = [
        "network: Network connectivity lost",
    ]
    patterns_network_connected = [
        "network: Network connectivity established",
    ]

    # Boot
    dut_board.uart.flush()
    reset_device()
    dut_board.uart.wait_for_str(patterns_boot, timeout=120)

    # LTE disconnect
    dut_board.uart.flush()
    dut_board.uart.write("att_network disconnect\r\n")
    dut_board.uart.wait_for_str(patterns_network_disconnected, timeout=20)

    # LTE reconnect
    dut_board.uart.flush()
    dut_board.uart.write("att_network connect\r\n")
    dut_board.uart.wait_for_str(patterns_network_connected, timeout=120)
