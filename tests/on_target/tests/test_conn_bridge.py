##########################################################################################
# Copyright (c) 2024 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import pytest
import time
import os
import types
from utils.flash_tools import flash_device, recover_device, dfu_device, get_first_artifact_match
from utils.uart import Uart, wait_until_uart_available
from tests.conftest import get_uarts
import sys
sys.path.append(os.getcwd())
from utils.logger import get_logger

logger = get_logger()

NRF53_NET_HEX_FILE = get_first_artifact_match("artifacts/connectivity-bridge-*-thingy91x-nrf53-net.hex")
NRF53_APP_HEX_FILE = get_first_artifact_match("artifacts/connectivity-bridge-*-thingy91x-nrf53-app.hex")
NRF91_CUSTOM_APP_ZIP = "artifacts/usb_uart_bridge_app.zip"

UART_TIMEOUT = 60
CB_TIMEOUT = 60
LINE_LEN = 64
LINE_COUNT = 64
BLE_CONTROL_MESSAGE_RETRIES = 3
MAC_ADDR_FLASH_LOCATION = 0x7A000

SEGGER_NRF53 = os.getenv('SEGGER_NRF53')
SEGGER_NRF91 = os.getenv('SEGGER_NRF91')
CONNECTIVITY_BRIDGE_UART = "THINGY91X_" + os.getenv('UART_ID', "")



@pytest.fixture(scope="function")
def t91x_conn_bridge():
    '''
    This fixture initializes the nRF53 and nRF91 for connectivity bridge test.
    First the nRF53 is flashed with connectivity bridge fw.
    Then dfu is performed on nrf91 with custom test app.
    '''
    logger.info("Recovering nRF53 network core")
    recover_device(serial=SEGGER_NRF53, core='Network')
    logger.info("Recovering nRF53 application core")
    recover_device(serial=SEGGER_NRF53, core='Application')
    logger.info("Flashing nRF53 with connectivity bridge firmware")
    logger.info("Flashing nRF53 network core")
    flash_device(hexfile=NRF53_NET_HEX_FILE, serial=SEGGER_NRF53, extra_args=['--core', 'Network', '--options', 'reset=RESET_NONE,chip_erase_mode=ERASE_ALL,verify=VERIFY_NONE'])
    logger.info("Flashing nRF53 application core")
    flash_device(hexfile=NRF53_APP_HEX_FILE, serial=SEGGER_NRF53, extra_args=['--options', 'reset=RESET_SYSTEM,chip_erase_mode=ERASE_ALL,verify=VERIFY_NONE'])
    logger.info("Waiting for connectivity bridge UART to be available")
    wait_until_uart_available(CONNECTIVITY_BRIDGE_UART)

    logger.info("nRF53 initialized successfully")

    logger.info("Flashing nrf91 with custom app through dfu")
    dfu_device(NRF91_CUSTOM_APP_ZIP, serial=CONNECTIVITY_BRIDGE_UART)

    all_uarts = get_uarts()
    if not all_uarts:
        pytest.fail("No UARTs found")
    logger.info(f"All uarts discovered: {all_uarts}")
    uart0_string = all_uarts[0]
    uart1_string = all_uarts[1]
    logger.info(f"UART0: {uart0_string}")
    logger.info(f"UART1: {uart1_string}")

    uart0 = Uart(uart0_string, name="Thingy91_UART0")
    uart1 = Uart(uart1_string, name="Thingy91_UART1", baudrate=1000000)

    yield types.SimpleNamespace(uart0=uart0, uart1=uart1)

    uart0.stop()
    uart1.stop()


@pytest.mark.dut2
@pytest.mark.conn_bridge
def test_conn_bridge(t91x_conn_bridge):
    t91x_conn_bridge.uart0.wait_for_str(
            "UART0 running at baudrate 115200", timeout=UART_TIMEOUT
        )
    t91x_conn_bridge.uart1.wait_for_str(
            "UART1 running at baudrate 1000000", timeout=UART_TIMEOUT
        )
    for i in range(BLE_CONTROL_MESSAGE_RETRIES):
        logger.debug(f"Requesting UART0 control message try: {i + 1}")
        t91x_conn_bridge.uart0.write(b"CHECK_UART0_SMOKE\r\n")
        try:
            t91x_conn_bridge.uart0.wait_for_str(
                "This message should be seen on UART0!",
                timeout=UART_TIMEOUT,
            )
            break
        except AssertionError:
            continue
    else:
        raise AssertionError(
            f"Control message not seen on UART0 after {BLE_CONTROL_MESSAGE_RETRIES} retries"
        )


    # Test large data tx on uart0 from usb to nrf9160
    # Emulates 4k of certificates writing to modem
    logger.info("Testing 4k of certificates writing to modem")
    t91x_conn_bridge.uart0.flush()
    ctrl_str = generate_control_str()
    ctrl_str = ctrl_str + '\n'
    ctrl_str_in_bytes = ctrl_str.encode("utf-8")

    logger.debug(f"Sending {len(ctrl_str_in_bytes)} bytes of data from usb over UART0")
    t91x_conn_bridge.uart0.write(ctrl_str_in_bytes)

    t91x_conn_bridge.uart0.wait_for_str('Control string received from usb via bridge to nrf9160!', timeout=10)
    logger.debug(f"{len(ctrl_str_in_bytes)} bytes of data sent from usb and received to nrf9160")


    # Test large data tx on uart1 from nrf9160 to usb
    # Emulates 4k of uart1 modem traces
    logger.info("Testing 4k of uart1 modem traces")
    ctrl_str = generate_control_str()
    t91x_conn_bridge.uart1.flush()
    t91x_conn_bridge.uart0.write(b"CHECK_UART1_4k_TRACES\r\n")
    try:
        t91x_conn_bridge.uart1.wait_for_str(ctrl_str, timeout=UART_TIMEOUT)
    except Exception as e:
        logger.error(f"wait_for_str failed, exception: {e}")
        log = t91x_conn_bridge.uart1.log.split()
        ctrl_str = ctrl_str.split()
        count = 0
        for count, log_line in enumerate(log):
            if ctrl_str[0] in log_line:
                break
        else:
            raise AssertionError(
                "first control line not found in log:\n" + ctrl_str[0]
            ) from AssertionError
        for line in ctrl_str:
            assert line in log[count], line + " not in " + log[count]
            count = count + 1

    # Test beefy tx on uart1 from nrf9160 to usb
    # Emulates 40k of uart1 modem traces (10 times the 4k control string)
    logger.info("Testing 40k of uart1 modem traces")
    beefy_ctrl_str = ''.join([generate_control_str() for _ in range(10)])  # Generate 10 times the control string
    t91x_conn_bridge.uart1.flush()
    t91x_conn_bridge.uart0.write(b"CHECK_UART1_40k_TRACES\r\n")
    t91x_conn_bridge.uart1.wait_for_str(beefy_ctrl_str, timeout=20)

    # TODO: refine assertion handling, see below for inspiration
    # try:
    #     t91x_conn_bridge.uart1.wait_for_str(beefy_ctrl_str, timeout=20)  # Wait for the null-terminated string
    # except Exception as e:
    #     logger.error(f"wait_for_str failed, exception: {e}")

    #     log = t91x_conn_bridge.uart1.log.split()
    #     beefy_ctrl_str = beefy_ctrl_str.split('\x00')  # Split the control string by null-terminator

    #     missing_strings = []  # List to keep track of missing control strings
    #     missing_strings_counter = 0
    #     for ctrl in beefy_ctrl_str:
    #         if ctrl not in log:
    #             missing_strings.append(ctrl)  # Add missing control string to the list
    #             missing_strings_counter = missing_strings_counter + 1

    #     if missing_strings:
    #         logger.error("LOG")
    #         logger.error(log)
    #         raise AssertionError(
    #             f"The following {missing_strings_counter} control strings were not found in the log:\n" + "\n".join(missing_strings)
    #         ) from AssertionError

def generate_control_str():
    control_str = ""
    for i in range(LINE_COUNT):
        for j in range(i, i + LINE_LEN - 1):
            c = chr(33 + j % LINE_LEN)
            c = "a" if c in ['"', "'", "\\"] else c
            control_str = control_str + c
        control_str = control_str + 'A'
    return control_str[:-1]
