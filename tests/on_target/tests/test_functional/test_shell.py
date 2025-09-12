##########################################################################################
# Copyright (c) 2025 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import os
import time
from utils.flash_tools import flash_device, reset_device
import sys
sys.path.append(os.getcwd())
from utils.logger import get_logger

logger = get_logger()

CLOUD_TIMEOUT = 60 * 3

def test_shell(dut_cloud, hex_file):
    '''
    Test that the device is operating normally using shell commands
    '''
    flash_device(os.path.abspath(hex_file))
    dut_cloud.uart.xfactoryreset()

    patterns_boot = [
        "main: passthrough_connected_waiting_entry: Passthrough mode: next trigger in",
    ]
    patterns_button_press = [
        "main: passthrough_connected_sampling_entry: passthrough_connected_sampling_entry",
    ]
    patterns_cloud_publish = [
        'Sending on payload channel: {"messageType":"DATA","appId":"donald","data":"duck"',
    ]
    patterns_network_disconnected = [
        "network: Network connectivity lost",
    ]
    patterns_network_connected = [
        "network: Network connectivity established",
    ]

    # Boot
    dut_cloud.uart.flush()
    reset_device()
    dut_cloud.uart.wait_for_str_with_retries("Connected to Cloud", max_retries=3, timeout=240, reset_func=reset_device)
    dut_cloud.uart.wait_for_str(patterns_boot, timeout=120)

    # Button press
    dut_cloud.uart.flush()
    dut_cloud.uart.write("att_button_press 1\r\n")
    dut_cloud.uart.wait_for_str(patterns_button_press, timeout=20)

    # Cloud publish
    dut_cloud.uart.flush()
    dut_cloud.uart.write("att_cloud_publish donald duck\r\n")
    dut_cloud.uart.wait_for_str(patterns_cloud_publish, timeout=20)

    messages = dut_cloud.cloud.get_messages(dut_cloud.device_id, appname="donald", max_records=20, max_age_hrs=0.25)

    # Wait for message to be reported to cloud
    start = time.time()
    while time.time() - start < CLOUD_TIMEOUT:
            time.sleep(5)
            messages = dut_cloud.cloud.get_messages(dut_cloud.device_id, appname="donald", max_records=20, max_age_hrs=0.25)
            logger.debug(f"Found messages: {messages}")

            latest_message = messages[0] if messages else None
            if latest_message:
                check_message_age = dut_cloud.cloud.check_message_age(message=latest_message, seconds=30)
                if check_message_age:
                    break
            else:
                logger.debug("No message with recent timestamp, retrying...")
                continue
    else:
        raise RuntimeError("No new message to cloud observed")

    # LTE disconnect
    dut_cloud.uart.flush()
    dut_cloud.uart.write("att_network disconnect\r\n")
    dut_cloud.uart.wait_for_str(patterns_network_disconnected, timeout=20)

    # LTE reconnect
    dut_cloud.uart.flush()
    dut_cloud.uart.write("att_network connect\r\n")
    dut_cloud.uart.wait_for_str(patterns_network_connected, timeout=120)
