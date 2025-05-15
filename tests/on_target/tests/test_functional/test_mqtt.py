##########################################################################################
# Copyright (c) 2024 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import os
from utils.flash_tools import flash_device, reset_device
import sys
import pytest

sys.path.append(os.getcwd())
from utils.logger import get_logger

logger = get_logger()

@pytest.mark.slow
def test_mqtt_firmware(dut_board, hex_file_mqtt):
    """Test the firmware with cloud MQTT module."""
    # Only run this test for thingy91x devices
    devicetype = os.getenv("DUT_DEVICE_TYPE")
    if devicetype != "thingy91x":
        pytest.skip("This test is only for thingy91x devices")

    # Flash the firmware
    flash_device(os.path.abspath(hex_file_mqtt))
    dut_board.uart.xfactoryreset()

    # Log patterns to check
    pattern_connect_to_broker = "network: Network connectivity established"
    pattern_cloud = "cloud: on_mqtt_connack: MQTT connection established, session present: 0"

    # Cloud connection
    dut_board.uart.flush()
    reset_device()

    dut_board.uart.wait_for_str_with_retries(pattern_cloud, max_retries=3, timeout=240, reset_func=reset_device)

    # Wait for shadow request
    dut_board.uart.wait_for_str(pattern_connect_to_broker, timeout=60)

    dut_board.uart.write("att_cloud_publish_mqtt test-payload\r\n")

    pattern_publish_ack = "cloud: on_mqtt_puback: Publish acknowledgment received, message id: 1"

    # Wait for publish acknowledgment for the test payload
    dut_board.uart.wait_for_str(pattern_publish_ack, timeout=30)
