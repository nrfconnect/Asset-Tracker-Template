##########################################################################################
# Copyright (c) 2024 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import subprocess
import os
import sys
import glob
import time
sys.path.append(os.getcwd())
from utils.logger import get_logger

logger = get_logger()

SEGGER = os.getenv('SEGGER')

RECOVER_MAX_ATTEMPTS = 3
RECOVER_RETRY_DELAY_SECONDS = 5

# A recover that fails part way through leaves the debug access port closed, which makes every
# later program attempt fail until the device is recovered again.
PROTECTION_ERROR_MARKERS = (
    "NotAvailableBecauseProtection",
    "AP-Protect",
    "readback protection",
)

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

def _is_protection_error(stderr):
    return any(marker in (stderr or "") for marker in PROTECTION_ERROR_MARKERS)

def _program_device(hexfile, serial, extra_args):
    subprocess.run(
        ['nrfutil', 'device', 'program', *extra_args, '--firmware', hexfile,
         '--serial-number', serial],
        check=True,
        text=True,
        capture_output=True
    )

def flash_device(hexfile, serial=SEGGER, extra_args=[]):
    # hexfile (str): Full path to file (hex or zip) to be programmed
    if not isinstance(hexfile, str):
        raise ValueError("hexfile cannot be None")
    logger.info(f"Flashing device, segger: {serial}, firmware: {hexfile}")
    try:
        _program_device(hexfile, serial, extra_args)
        logger.info("Command completed successfully.")
    except subprocess.CalledProcessError as e:
        # Handle errors in the command execution
        logger.info("An error occurred while flashing the device.")
        logger.info("Error output:")
        logger.info(e.stderr)
        if not _is_protection_error(e.stderr):
            raise
        logger.warning("Access port is closed, recovering the device and flashing again")
        recover_device(serial)
        try:
            _program_device(hexfile, serial, extra_args)
            logger.info("Command completed successfully.")
        except subprocess.CalledProcessError as retry_error:
            logger.info("Flashing failed again after recovering the device.")
            logger.info("Error output:")
            logger.info(retry_error.stderr)
            raise

    reset_device(serial)

def recover_device(serial=SEGGER, core="Application", max_attempts=RECOVER_MAX_ATTEMPTS):
    for attempt in range(1, max_attempts + 1):
        logger.info(f"Recovering device, segger: {serial}")
        try:
            subprocess.run(
                ['nrfutil', 'device', 'recover', '--serial-number', serial, '--core', core],
                check=True,
                text=True,
                capture_output=True
            )
            logger.info("Command completed successfully.")
            return
        except subprocess.CalledProcessError as e:
            # Handle errors in the command execution
            logger.info("An error occurred while recovering the device.")
            logger.info("Error output:")
            logger.info(e.stderr)
            if attempt == max_attempts:
                raise
            logger.warning(f"Retrying recover, attempt {attempt + 1}/{max_attempts}")
            time.sleep(RECOVER_RETRY_DELAY_SECONDS)

def get_first_artifact_match(pattern):
    matches = glob.glob(pattern)
    if matches:
        return matches[0]
    else:
        return None
