##########################################################################################
# Copyright (c) 2025 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import pytest
import os
import requests
import datetime
import time
from utils.flash_tools import flash_device, reset_device
from utils.logger import get_logger

MEMFAULT_ORG_TOKEN = os.getenv('MEMFAULT_ORGANIZATION_TOKEN')
MEMFAULT_ORG = os.getenv('MEMFAULT_ORGANIZATION_SLUG')
MEMFAULT_PROJ = os.getenv('MEMFAULT_PROJECT_SLUG')
IMEI = os.getenv('IMEI')
MEMFAULT_TIMEOUT = 5 * 60

logger = get_logger()

url = "https://api.memfault.com/api/v0"
auth = ("", MEMFAULT_ORG_TOKEN)

def fetch_recent_modem_trace(device_id, time_span_minutes):
    """
    Fetch the ID of the most recent modem trace job from the device if its start time
    is within the specified time span.

    Parameters:
        device_id (str): The serial ID of the device.
        time_span_minutes (int): The time span in minutes within which the modem trace should have started.

    Returns:
        int or None: The ID of the modem trace job if the conditions are met, otherwise None.
    """

    r = requests.get(
        f"{url}/organizations/{MEMFAULT_ORG}/projects/{MEMFAULT_PROJ}/devices/{device_id}/custom-data-recordings?page=1&per_page=1",
        auth=auth
    )
    r.raise_for_status()
    response = r.json()

    # Extract relevant data from the response
    data = response.get("data", [])
    for recording in data:
        # Verify that the start time and other conditions are met
        start_time_str = recording.get("start_time")
        if start_time_str:
            start_time = datetime.fromisoformat(start_time_str.replace("Z", "+00:00"))  # Process ISO 8601 UTC time
            valid_time_threshold = datetime.utcnow() - timedelta(minutes=time_span_minutes)

            if start_time >= valid_time_threshold:  # Check if start time is within the given time span
                return recording.get("id")  # Return the ID of the job

    # Return None if no recording met the conditions
    return None

def get_traces(family, device_id):
    r = requests.get(
        f"{url}/organizations/{MEMFAULT_ORG}/projects/{MEMFAULT_PROJ}/traces", auth=auth)
    r.raise_for_status()
    data = r.json()["data"]
    latest_traces = [
        x
        for x in data
        if x["device"]["device_serial"] == str(device_id) and x["source_type"] == family
    ]
    return latest_traces

def get_latest_coredump_traces(device_id):
    return get_traces("coredump", device_id)

def timestamp(event):
    return datetime.datetime.strptime(
        event["captured_date"], "%Y-%m-%dT%H:%M:%S.%f%z"
    )

def test_memfault(dut_board, hex_file):
    # Save timestamp of latest coredump
    coredumps = get_latest_coredump_traces(IMEI)
    logger.debug(f"Found coredumps: {coredumps}")
    timestamp_old_coredump = timestamp(coredumps[0]) if coredumps else  None
    logger.debug(f"Timestamp old coredump: {timestamp_old_coredump}")


    flash_device(os.path.abspath(hex_file))
    dut_board.uart.xfactoryreset()
    dut_board.uart.flush()
    reset_device()
    dut_board.uart.wait_for_str("Connected to Cloud", timeout=120)

    time.sleep(10)

    # Trigger usage fault to generate coredump
    dut_board.uart.write("mflt test usagefault\r\n")

    # Wait for upload to be reported to memfault api
    start = time.time()
    while time.time() - start < MEMFAULT_TIMEOUT:
            time.sleep(5)
            coredumps = get_latest_coredump_traces(IMEI)
            logger.debug(f"Found coredumps: {coredumps}")
            timestamp_new_coredump = timestamp(coredumps[0]) if coredumps else  None
            logger.debug(f"Timestamp new coredump: {timestamp_new_coredump}")

            if not coredumps:
                continue
            # Check that we have an upload with newer timestamp
            if not timestamp_old_coredump:
                break
            if timestamp(coredumps[0]) > timestamp_old_coredump:
                break
    else:
        raise RuntimeError("No new coredump observed")

    # Wait for modem trace to be reported to memfault api
    start = time.time()

    while time.time() - start < MEMFAULT_TIMEOUT:
        modem_trace_id = fetch_recent_modem_trace(IMEI, 5)
        if modem_trace_id:
            print(f"Found modem trace with ID {modem_trace_id}")
            break
        time.sleep(5)
    else:
        raise RuntimeError("No modem trace observed")
