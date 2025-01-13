##########################################################################################
# Copyright (c) 2024 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import subprocess
import os
import sys
import glob
sys.path.append(os.getcwd())
from utils.logger import get_logger

logger = get_logger()

SEGGER = os.getenv('SEGGER')

def reset_device(serial=SEGGER, reset_kind="RESET_SYSTEM"):
    logger.info(f"Resetting device, segger: {serial}")
    try:
        result = subprocess.run(
            ['nrfutil', 'device', 'reset', '--serial-number', serial, '--reset-kind', reset_kind],
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

def flash_device(hexfile, serial=SEGGER, extra_args=[]):
    # hexfile (str): Full path to file (hex or zip) to be programmed
    if not isinstance(hexfile, str):
        raise ValueError("hexfile cannot be None")
    logger.info(f"Flashing device, segger: {serial}, firmware: {hexfile}")
    try:
        result = subprocess.run(['nrfutil', 'device', 'program', *extra_args, '--firmware', hexfile, '--serial-number', serial], check=True, text=True, capture_output=True)
        logger.info("Command completed successfully.")
    except subprocess.CalledProcessError as e:
        # Handle errors in the command execution
        logger.info("An error occurred while flashing the device.")
        logger.info("Error output:")
        logger.info(e.stderr)
        raise

    reset_device(serial)

def recover_device(serial=SEGGER, core="Application"):
    logger.info(f"Recovering device, segger: {serial}")
    try:
        result = subprocess.run(['nrfutil', 'device', 'recover', '--serial-number', serial, '--core', core], check=True, text=True, capture_output=True)
        logger.info("Command completed successfully.")
    except subprocess.CalledProcessError as e:
        # Handle errors in the command execution
        logger.info("An error occurred while recovering the device.")
        logger.info("Error output:")
        logger.info(e.stderr)
        raise

def get_first_artifact_match(pattern):
    matches = glob.glob(pattern)
    if matches:
        return matches[0]
    else:
        return None
