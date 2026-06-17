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
from utils.nrfcloud import NRFCloud

logger = get_logger()

UART_TIMEOUT = 60 * 30

SEGGER = os.getenv('SEGGER')
UART_ID = os.getenv('UART_ID', SEGGER)
DEVICE_UUID = os.getenv('UUID')
NRFCLOUD_API_KEY = os.getenv('NRFCLOUD_API_KEY')
DUT_DEVICE_TYPE = os.getenv('DUT_DEVICE_TYPE')

NRFCLOUD_ORG_TOKEN = os.getenv('MEMFAULT_ORGANIZATION_TOKEN')
NRFCLOUD_ORG = os.getenv('MEMFAULT_ORGANIZATION_SLUG')
NRFCLOUD_PROJECT = os.getenv('MEMFAULT_PROJECT_SLUG')
NRFCLOUD_COHORT = os.getenv('MEMFAULT_OTA_COHORT') or 'default'
NRFCLOUD_HW_VERSION = os.getenv('MEMFAULT_HW_VERSION') or DUT_DEVICE_TYPE
NRFCLOUD_APP_SOFTWARE_TYPE = os.getenv('MEMFAULT_APP_SOFTWARE_TYPE') or 'app'

NRFCLOUD_MODEM_PROJECT = os.getenv('MEMFAULT_MODEM_PROJECT_SLUG')
NRFCLOUD_MODEM_ORG_TOKEN = os.getenv('MEMFAULT_MODEM_ORGANIZATION_TOKEN') or NRFCLOUD_ORG_TOKEN
NRFCLOUD_MODEM_ORG = os.getenv('MEMFAULT_MODEM_ORGANIZATION_SLUG') or NRFCLOUD_ORG
NRFCLOUD_MODEM_COHORT = os.getenv('MEMFAULT_MODEM_OTA_COHORT') or NRFCLOUD_COHORT
NRFCLOUD_MODEM_HW_VERSION = os.getenv('MEMFAULT_MODEM_HW_VERSION') or DUT_DEVICE_TYPE

def get_uarts():
    # Handle platform-specific serial device paths
    import platform

    if platform.system() == "Darwin":  # macOS
        base_path = "/dev"
    else:  # Linux
        base_path = "/dev/serial/by-id"

    try:
        if platform.system() == "Darwin":
            serial_paths = [os.path.join(base_path, entry) for entry in os.listdir(base_path)
                          if entry.startswith("tty.")]
            logger.info(f"Found serial devices: {serial_paths}")
        else:
            serial_paths = [os.path.join(base_path, entry) for entry in os.listdir(base_path)]
    except (FileNotFoundError, PermissionError) as e:
        raise RuntimeError("Failed to list serial devices") from e
    if not UART_ID:
        raise RuntimeError("UART_ID not set")
    uarts = [x for x in sorted(serial_paths) if UART_ID in x]
    logger.info(f"Found UARTs: {uarts}")
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
    if uart._serial_exception_count:
        logger.warning(
            f"UART SerialException count for test: {uart._serial_exception_count}"
        )
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
    if not (NRFCLOUD_ORG_TOKEN and NRFCLOUD_ORG and NRFCLOUD_PROJECT):
        pytest.skip("OTA organization/project environment variables not set")

    fota = NRFCloud(
        api_key=NRFCLOUD_API_KEY,
        nrfcloud_org_token=NRFCLOUD_ORG_TOKEN,
        nrfcloud_org=NRFCLOUD_ORG,
        nrfcloud_project=NRFCLOUD_PROJECT)

    # Modem firmware uses a separate OTA project; only wire it up when
    # configured, otherwise skip tests
    modem_ota = None
    if NRFCLOUD_MODEM_PROJECT and NRFCLOUD_MODEM_ORG_TOKEN and NRFCLOUD_MODEM_ORG:
        modem_ota = NRFCloud(
            api_key=NRFCLOUD_API_KEY,
            nrfcloud_org_token=NRFCLOUD_MODEM_ORG_TOKEN,
            nrfcloud_org=NRFCLOUD_MODEM_ORG,
            nrfcloud_project=NRFCLOUD_MODEM_PROJECT)

    yield types.SimpleNamespace(
        **dut_board.__dict__,
        fota=fota,
        modem_ota=modem_ota,
        device_id=DEVICE_UUID,
        cohort=NRFCLOUD_COHORT,
        modem_cohort=NRFCLOUD_MODEM_COHORT,
        hw_version=NRFCLOUD_HW_VERSION,
        modem_hw_version=NRFCLOUD_MODEM_HW_VERSION,
        app_software_type=NRFCLOUD_APP_SOFTWARE_TYPE,
    )

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
    # Skip if not thingy91x since debug build is only available for thingy91x
    if DUT_DEVICE_TYPE != 'thingy91x':
        pytest.skip("Debug build is only available for thingy91x")

    # Search for the debug firmware hex file in the artifacts folder
    artifacts_dir = "artifacts/"
    hex_pattern = f"asset-tracker-template-{r'[0-9a-z\.]+'}-debug-{DUT_DEVICE_TYPE}-nrf91.hex"

    for file in os.listdir(artifacts_dir):
        if re.match(hex_pattern, file):
            return os.path.join(artifacts_dir, file)

    pytest.fail("No matching debug firmware .hex file found in the artifacts directory")

@pytest.fixture(scope="session")
def hex_file_patched():
    # Skip if not thingy91x since patched build is only available for thingy91x
    if DUT_DEVICE_TYPE != 'thingy91x':
        pytest.skip("Patched build is only available for thingy91x")

    # Search for the firmware hex file in the artifacts folder
    artifacts_dir = "artifacts/"
    hex_pattern = f"asset-tracker-template-{r"[0-9a-z\.]+"}-patched-{DUT_DEVICE_TYPE}-nrf91.hex"

    for file in os.listdir(artifacts_dir):
        if re.match(hex_pattern, file):
            return os.path.join(artifacts_dir, file)

    pytest.fail("No matching firmware .hex file found in the artifacts directory")

@pytest.fixture(scope="session")
def hex_file_mqtt():
    # Skip if not thingy91x since MQTT build is only available for thingy91x
    if DUT_DEVICE_TYPE != 'thingy91x':
        pytest.skip("mqtt build is only available for thingy91x")

    # Search for the firmware hex file in the artifacts folder
    artifacts_dir = "artifacts/"
    hex_pattern = f"asset-tracker-template-{r"[0-9a-z\.]+"}-mqtt-{DUT_DEVICE_TYPE}-nrf91.hex"

    for file in os.listdir(artifacts_dir):
        if re.match(hex_pattern, file):
            return os.path.join(artifacts_dir, file)

    pytest.fail("No matching firmware .hex file found in the artifacts directory")

@pytest.fixture(scope="session")
def hex_file_ext_gnss():
    # Skip if not nrf9151dk since external GNSS build is only available for nrf9151dk
    if DUT_DEVICE_TYPE != 'nrf9151dk':
        pytest.skip("External GNSS build is only available for nrf9151dk")

    # Search for the firmware hex file in the artifacts folder
    artifacts_dir = "artifacts/"
    hex_pattern = f"asset-tracker-template-{r'[0-9a-z\.]+'}-ext-gnss-{DUT_DEVICE_TYPE}-nrf91.hex"

    for file in os.listdir(artifacts_dir):
        if re.match(hex_pattern, file):
            return os.path.join(artifacts_dir, file)

    pytest.fail("No matching external GNSS firmware .hex file found in the artifacts directory")

@pytest.fixture(scope="session")
def hex_file_buffer_ram():
    # Skip if not thingy91x since buffer RAM build is only available for thingy91x
    if DUT_DEVICE_TYPE != 'thingy91x':
        pytest.skip("Buffer RAM build is only available for thingy91x")

    # Search for the firmware hex file in the artifacts folder
    artifacts_dir = "artifacts/"
    hex_pattern = f"asset-tracker-template-{r'[0-9a-z\.]+'}-buffer-ram-{DUT_DEVICE_TYPE}-nrf91.hex"

    for file in os.listdir(artifacts_dir):
        if re.match(hex_pattern, file):
            return os.path.join(artifacts_dir, file)

    pytest.fail("No matching buffer RAM firmware .hex file found in the artifacts directory")

@pytest.fixture(scope="session")
def hex_file_buffer_flash():
    # Skip if not thingy91x since buffer flash build is only available for thingy91x
    if DUT_DEVICE_TYPE != 'thingy91x':
        pytest.skip("Buffer flash build is only available for thingy91x")

    # Search for the firmware hex file in the artifacts folder
    artifacts_dir = "artifacts/"
    hex_pattern = f"asset-tracker-template-{r'[0-9a-z\.]+'}-buffer-flash-{DUT_DEVICE_TYPE}-nrf91.hex"

    for file in os.listdir(artifacts_dir):
        if re.match(hex_pattern, file):
            return os.path.join(artifacts_dir, file)

    pytest.fail("No matching buffer flash firmware .hex file found in the artifacts directory")
