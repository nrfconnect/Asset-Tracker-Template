##########################################################################################
# Copyright (c) 2024 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import pytest
import time
import os
from utils.flash_tools import flash_device, reset_device
import sys
sys.path.append(os.getcwd())
from utils.logger import get_logger

logger = get_logger()

trace_collection_time = 60
trace_read_timeout = 60
threshold_lost_traces = 200

@pytest.mark.traces
@pytest.mark.dut1
def test_traces(t91x_traces, hex_file):
    flash_device(os.path.abspath(hex_file))
    time.sleep(5)
    t91x_traces.uart.xfactoryreset()
    t91x_traces.uart.flush()
    reset_device()

    t91x_traces.uart.wait_for_str("nrf_modem_lib_trace: Trace thread ready", timeout=60)
    t91x_traces.uart.write("modem_trace size\r\n")
    t91x_traces.uart.wait_for_str("Modem trace data size:", timeout=5)
    modem_trace_size_1 = t91x_traces.uart.extract_value(r"Modem trace data size: (\d+) bytes")
    assert modem_trace_size_1

    logger.info("Collecting modem traces for 60 seconds")
    time.sleep(trace_collection_time)
    t91x_traces.uart.write("modem_trace stop\r\n")
    log_len = t91x_traces.uart.wait_for_str("Modem trace stop command issued. This may produce a few more traces. Please wait at least 1 second before attempting to read out trace data.", timeout=10)
    time.sleep(1)
    t91x_traces.uart.write("modem_trace size\r\n")
    t91x_traces.uart.wait_for_str("Modem trace data size:", timeout=5, start_pos=log_len)
    modem_trace_size_2 = t91x_traces.uart.extract_value(r"Modem trace data size: (\d+) bytes", start_pos=log_len)
    assert modem_trace_size_2
    assert int(modem_trace_size_2[0]) > int(modem_trace_size_1[0])

    logger.info("Dumping modem traces to uart1")
    uart1_before_dump = t91x_traces.trace.get_size()
    t91x_traces.uart.write("modem_trace dump_uart\r\n")
    t91x_traces.uart.wait_for_str_ordered(["Reading out", "bytes of trace data"], timeout=5)
    modem_trace_size_to_read = t91x_traces.uart.extract_value(r"Reading out (\d+) bytes of trace data")
    assert modem_trace_size_to_read
    assert int(modem_trace_size_to_read[0]) == int(modem_trace_size_2[0])
    start_t = time.time()
    while True:
        uart1_size = t91x_traces.trace.get_size()
        logger.debug(f"Uart1 trace size {uart1_size} bytes after {time.time()-start_t} seconds")
        if uart1_size - uart1_before_dump == int(modem_trace_size_to_read[0]):
            break
        if start_t + trace_read_timeout < time.time():
            if abs(uart1_size - uart1_before_dump - int(modem_trace_size_to_read[0])) < threshold_lost_traces:
                 logger.warning(f"Uart1 trace size {uart1_size}, expected {int(modem_trace_size_to_read[0]) + uart1_before_dump}\n \
                                uart1 trace size before dump: {uart1_before_dump}")
                 break
            raise AssertionError(f"Timeout waiting for uart1 trace size to match the expected size \n \
                                Expected size: {int(modem_trace_size_to_read[0]) + uart1_before_dump} bytes \n \
                                Actual size: {uart1_size} bytes \n \
                                uart1 trace size before dump: {uart1_before_dump} bytes")
        time.sleep(1)
    modem_trace_size_read = t91x_traces.uart.extract_value(r"Total trace bytes read from flash: (\d+)")
    assert modem_trace_size_read
    assert int(modem_trace_size_read[0]) == int(modem_trace_size_to_read[0])
