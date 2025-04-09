##########################################################################################
# Copyright (c) 2024 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import os
import re
import pytest
import types
from utils.flash_tools import recover_device
from utils.uart import Uart, UartBinary
import sys
sys.path.append(os.getcwd())
from utils.logger import get_logger
from utils.nrfcloud import NRFCloud, NRFCloudFOTA

logger = get_logger()

UART_TIMEOUT = 60 * 30

SEGGER = os.getenv('SEGGER')
UART_ID = os.getenv('UART_ID', SEGGER)
DEVICE_UUID = os.getenv('UUID')
NRFCLOUD_API_KEY = os.getenv('NRFCLOUD_API_KEY')
DUT_DEVICE_TYPE = os.getenv('DUT_DEVICE_TYPE')
if DUT_DEVICE_TYPE == "ppk_thingy91x":
    DUT_DEVICE_TYPE = "thingy91x"


def get_uarts():
    base_path = "/dev/serial/by-id"
    try:
        serial_paths = [os.path.join(base_path, entry) for entry in os.listdir(base_path)]
    except (FileNotFoundError, PermissionError) as e:
        logger.error(e)
        return False

    uarts = []

    for path in sorted(serial_paths):
        if UART_ID in path:
            uarts.append(path)
        else:
            continue
    return uarts

def scan_log_for_assertions(log):
    assert_counts = log.count("ASSERT")
    if assert_counts > 0:
        pytest.fail(f"{assert_counts} ASSERT found in log: {log}")

@pytest.hookimpl(tryfirst=True)
def pytest_runtest_logstart(nodeid, location):
    logger.info(f"Starting test: {nodeid}")

@pytest.hookimpl(trylast=True)
def pytest_runtest_logfinish(nodeid, location):
    logger.info(f"Finished test: {nodeid}")

@pytest.fixture(scope="function")
def dut_board():
    all_uarts = get_uarts()
    if not all_uarts:
        pytest.fail("No UARTs found")
    log_uart_string = all_uarts[0]
    uart = Uart(log_uart_string, timeout=UART_TIMEOUT)

    yield types.SimpleNamespace(
        uart=uart,
        device_type=DUT_DEVICE_TYPE
    )

    uart_log = uart.whole_log
    uart.stop()
    recover_device()

    scan_log_for_assertions(uart_log)

@pytest.fixture(scope="function")
def dut_cloud(dut_board):
    if not NRFCLOUD_API_KEY:
        pytest.skip("NRFCLOUD_API_KEY environment variable not set")
    if not DEVICE_UUID:
        pytest.skip("UUID environment variable not set")

    cloud = NRFCloud(api_key=NRFCLOUD_API_KEY)
    device_id = DEVICE_UUID

    yield types.SimpleNamespace(
        **dut_board.__dict__,
        cloud=cloud,
        device_id=device_id,
    )

@pytest.fixture(scope="function")
def dut_fota(dut_board):
    if not NRFCLOUD_API_KEY:
        pytest.skip("NRFCLOUD_API_KEY environment variable not set")
    if not DEVICE_UUID:
        pytest.skip("UUID environment variable not set")

    fota = NRFCloudFOTA(api_key=NRFCLOUD_API_KEY)
    device_id = DEVICE_UUID
    data = {
        'job_id': '',
    }
    fota.cancel_incomplete_jobs(device_id)

    yield types.SimpleNamespace(
        **dut_board.__dict__,
        fota=fota,
        device_id=device_id,
        data=data
    )
    fota.cancel_incomplete_jobs(device_id)


@pytest.fixture(scope="module")
def dut_traces(dut_board):
    all_uarts = get_uarts()
    trace_uart_string = all_uarts[1]
    uart_trace = UartBinary(trace_uart_string)

    yield types.SimpleNamespace(
        **dut_board.__dict__,
        trace=uart_trace,
        )

    uart_trace.stop()

@pytest.fixture(scope="session")
def hex_file():
    # Search for the firmware hex file in the artifacts folder
    artifacts_dir = "artifacts/"
    hex_pattern = f"asset-tracker-template-{r"[0-9a-z\.]+"}-{DUT_DEVICE_TYPE}-nrf91.hex"

    for file in os.listdir(artifacts_dir):
        if re.match(hex_pattern, file):
            return os.path.join(artifacts_dir, file)

    pytest.fail("No matching firmware .hex file found in the artifacts directory")

@pytest.fixture(scope="session")
def debug_hex_file():
    # Search for the debug firmware hex file in the artifacts folder
    artifacts_dir = "artifacts/"
    hex_pattern = f"asset-tracker-template-{r'[0-9a-z\.]+'}-debug-{DUT_DEVICE_TYPE}-nrf91.hex"

    for file in os.listdir(artifacts_dir):
        if re.match(hex_pattern, file):
            return os.path.join(artifacts_dir, file)

    pytest.fail("No matching debug firmware .hex file found in the artifacts directory")

@pytest.fixture(scope="session")
def bin_file():
    # Search for the firmware bin file in the artifacts folder
    artifacts_dir = "artifacts"
    hex_pattern = f"asset-tracker-template-{r"[0-9a-z\.]+"}-{DUT_DEVICE_TYPE}-nrf91-update-signed.hex"

    for file in os.listdir(artifacts_dir):
        if re.match(hex_pattern, file):
            return os.path.join(artifacts_dir, file)

    pytest.fail("No matching firmware .bin file found in the artifacts directory")
