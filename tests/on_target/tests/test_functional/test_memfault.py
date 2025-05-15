##########################################################################################
# Copyright (c) 2025 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import pytest
import os
import requests
import time
import subprocess
from utils.flash_tools import flash_device, reset_device
from utils.logger import get_logger
from datetime import datetime, timezone

MEMFAULT_ORG_TOKEN = os.getenv('MEMFAULT_ORGANIZATION_TOKEN')
MEMFAULT_ORG = os.getenv('MEMFAULT_ORGANIZATION_SLUG')
MEMFAULT_PROJ = os.getenv('MEMFAULT_PROJECT_SLUG')
IMEI = os.getenv('IMEI')
MEMFAULT_TIMEOUT = 5 * 60

logger = get_logger()

url = "https://api.memfault.com/api/v0"
auth = ("", MEMFAULT_ORG_TOKEN)

def convert_binary_trace_to_pcap(binary_trace, pcapng_file):
    logger.info(f"Converting modem trace to pcap")
    try:
        result = subprocess.run(
            ['nrfutil', 'trace', 'lte', '--input-file', binary_trace, '--output-pcapng', pcapng_file],
            check=True,
            text=True,
            capture_output=True
        )
        logger.info("Command completed successfully.")
    except subprocess.CalledProcessError as e:
        # Handle errors in the command execution
        logger.info("An error occurred while resetting the device.")
        logger.info("Error output:")
        logger.info(e.stderr)
        raise

def fetch_recent_modem_trace(device_id, start_time, end_time):
    r = requests.get(
        f"{url}/organizations/{MEMFAULT_ORG}/projects/{MEMFAULT_PROJ}/devices/{device_id}/custom-data-recordings?end_time={end_time}&page=1&per_page=1&start_time={start_time}",
        auth=auth
    )

    # Print the request URL for debugging
    logger.debug(f"Request URL: {r.url}")
    # Print the response status code for debugging
    logger.debug(f"Response status code: {r.status_code}")
    # Print the response content for debugging
    logger.debug(f"Response content: {r.content}")

    r.raise_for_status()
    response = r.json()

    # Extract relevant data from the response
    data = response.get("data", [])

    if data:
        recording = data[0]
        reason = recording.get("reason")

        if reason == "modem traces":
                return recording.get("id")

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
    return datetime.strptime(
        event["captured_date"], "%Y-%m-%dT%H:%M:%S.%f%z"
    )

def test_memfault(dut_board, debug_hex_file):
    # Save timestamp of latest coredump
    coredumps = get_latest_coredump_traces(IMEI)
    logger.debug(f"Found coredumps: {coredumps}")
    timestamp_old_coredump = timestamp(coredumps[0]) if coredumps else  None
    logger.debug(f"Timestamp old coredump: {timestamp_old_coredump}")

    flash_device(os.path.abspath(debug_hex_file))
    dut_board.uart.xfactoryreset()
    dut_board.uart.flush()
    reset_device()

    dut_board.uart.wait_for_str_with_retries("Connected to Cloud", max_retries=3, timeout=240, reset_func=reset_device)

    now = datetime.now(timezone.utc)
    start_time = now.strftime("%Y-%m-%dT%H:%M:%SZ")

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

        now = datetime.now(timezone.utc)
        end_time = now.strftime("%Y-%m-%dT%H:%M:%SZ")

        modem_trace_id = fetch_recent_modem_trace(IMEI, end_time, start_time)
        if modem_trace_id:
            print(f"Found modem trace with ID {modem_trace_id}")
            break
        time.sleep(5)
    else:
        raise RuntimeError("No modem trace observed")

    # Download modem trace
    r = requests.get(
        f"{url}/organizations/{MEMFAULT_ORG}/projects/{MEMFAULT_PROJ}/custom-data-recording/{modem_trace_id}/download",
        auth=auth
    )
    r.raise_for_status()

    # Save the binary trace to a file
    binary_trace_path = f"modem_trace_{modem_trace_id}.bin"
    with open(binary_trace_path, "wb") as f:
        f.write(r.content)
    logger.info(f"Saved modem trace to {binary_trace_path}")
    # Convert the binary trace to pcapng format
    pcapng_file = f"modem_trace_{modem_trace_id}.pcapng"
    convert_binary_trace_to_pcap(binary_trace_path, pcapng_file)
    logger.info(f"Converted modem trace to {pcapng_file}")
    # Clean up the binary trace file
    os.remove(binary_trace_path)
    logger.info(f"Removed {binary_trace_path}")
    # Check if the pcapng file exists
    assert os.path.exists(pcapng_file), f"Failed to create {pcapng_file}"
    logger.info(f"Successfully created {pcapng_file}")
    # Check if the pcapng file is not empty
    assert os.path.getsize(pcapng_file) > 0, f"{pcapng_file} is empty"
    logger.info(f"{pcapng_file} is not empty")
    # Clean up the pcapng file
    os.remove(pcapng_file)
    logger.info(f"Removed {pcapng_file}")
