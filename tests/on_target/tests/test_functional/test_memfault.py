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


def test_memfault(t91x_board, hex_file):
    # Save timestamp of latest coredump
    coredumps = get_latest_coredump_traces(IMEI)
    logger.debug(f"Found coredumps: {coredumps}")
    timestamp_old_coredump = timestamp(coredumps[0]) if coredumps else  None
    logger.debug(f"Timestamp old coredump: {timestamp_old_coredump}")


    flash_device(os.path.abspath(hex_file))
    t91x_board.uart.xfactoryreset()
    t91x_board.uart.flush()
    reset_device()
    t91x_board.uart.wait_for_str("Connected to Cloud", timeout=120)

    time.sleep(10)

    # Trigger usage fault to generate coredump
    t91x_board.uart.write("mflt test usagefault\r\n")

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
        raise RuntimeError(f"No new coredump observed")

