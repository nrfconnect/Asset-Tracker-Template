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
from utils.nrfcloud_fota import NRFCloudFOTA

logger = get_logger()

UART_TIMEOUT = 60 * 30

SEGGER = os.getenv('SEGGER')
UART_ID = os.getenv('UART_ID', SEGGER)
FOTADEVICE_UUID = os.getenv('UUID')
NRFCLOUD_API_KEY = os.getenv('NRFCLOUD_API_KEY')

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

@pytest.fixture(scope="module")
def t91x_board():
    all_uarts = get_uarts()
    if not all_uarts:
        pytest.fail("No UARTs found")
    log_uart_string = all_uarts[0]
    uart = Uart(log_uart_string, timeout=UART_TIMEOUT)

    yield types.SimpleNamespace(uart=uart)

    uart_log = uart.whole_log
    uart.stop()
    recover_device()

    scan_log_for_assertions(uart_log)

@pytest.fixture(scope="function")
def t91x_fota(t91x_board):
    if not NRFCLOUD_API_KEY:
        pytest.skip("NRFCLOUD_API_KEY environment variable not set")
    if not FOTADEVICE_UUID:
        pytest.skip("UUID environment variable not set")

    fota = NRFCloudFOTA(api_key=NRFCLOUD_API_KEY)
    device_id = FOTADEVICE_UUID
    data = {
        'job_id': '',
        'bundle_id': ''
    }
    fota.cancel_incomplete_jobs(device_id)

    yield types.SimpleNamespace(
        fota=fota,
        uart=t91x_board.uart,
        device_id=device_id,
        data=data
    )

    fota.cancel_incomplete_jobs(device_id)
    if data['bundle_id']:
        fota.delete_bundle(data['bundle_id'])


@pytest.fixture(scope="module")
def t91x_traces(t91x_board):
    all_uarts = get_uarts()
    trace_uart_string = all_uarts[1]
    uart_trace = UartBinary(trace_uart_string)

    yield types.SimpleNamespace(
        trace=uart_trace,
        uart=t91x_board.uart
        )

    uart_trace.stop()

@pytest.fixture(scope="session")
def hex_file():
    # Search for the firmware hex file in the artifacts folder
    artifacts_dir = "artifacts"
    hex_pattern = r"asset-tracker-template-[0-9a-z\.]+-thingy91x-nrf91\.hex"

    for file in os.listdir(artifacts_dir):
        if re.match(hex_pattern, file):
            return os.path.join(artifacts_dir, file)

    pytest.fail("No matching firmware .hex file found in the artifacts directory")

@pytest.fixture(scope="session")
def bin_file():
    # Search for the firmware bin file in the artifacts folder
    artifacts_dir = "artifacts"
    hex_pattern = r"asset-tracker-template-[0-9a-z\.]+-thingy91x-nrf91-update-signed\.bin"

    for file in os.listdir(artifacts_dir):
        if re.match(hex_pattern, file):
            return os.path.join(artifacts_dir, file)

    pytest.fail("No matching firmware .bin file found in the artifacts directory")
