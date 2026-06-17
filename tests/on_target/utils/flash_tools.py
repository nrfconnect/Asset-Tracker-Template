##########################################################################################
# Copyright (c) 2024 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import re
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

# tests/on_target/utils -> project/app, and workspace root/build_app_fota_update
APP_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "app"))
APP_UPDATE_BUILD_DIR = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "..", "..", "build_app_fota_update"))
MODEM_FOTA_BUILD_DIR = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "..", "..", "build_modem_fota"))

MEMFAULT_FOTA_MODEM_PROJECT_KEY_ENV = "MEMFAULT_FOTA_MODEM_PROJECT_KEY"

BOARD_BY_DEVICE_TYPE = {
    "thingy91x": "thingy91x/nrf9151/ns",
    "nrf9151dk": "nrf9151dk/nrf9151/ns",
}

_VERSION_FIELD_RE = re.compile(r"^(VERSION_MAJOR|VERSION_MINOR|PATCHLEVEL)\s*=\s*(\d+)")

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

def read_app_version(app_dir=APP_DIR):
    """Parse the app's Zephyr VERSION file, returning (major, minor, patch).

    EXTRAVERSION/VERSION_TWEAK are ignored: they don't factor into whether one
    release is newer than another for our purposes here.
    """
    values = {}
    with open(os.path.join(app_dir, "VERSION")) as f:
        for line in f:
            match = _VERSION_FIELD_RE.match(line.strip())
            if match:
                values[match.group(1)] = int(match.group(2))

    missing = {"VERSION_MAJOR", "VERSION_MINOR", "PATCHLEVEL"} - values.keys()
    if missing:
        raise RuntimeError(f"VERSION file at {app_dir} is missing fields: {sorted(missing)}")
    return values["VERSION_MAJOR"], values["VERSION_MINOR"], values["PATCHLEVEL"]

def next_app_version(app_dir=APP_DIR):
    """Return a semver strictly greater than the app's current VERSION file."""
    major, minor, patch = read_app_version(app_dir)
    return f"{major}.{minor}.{patch + 1}"

def _resolve_board(device_type):
    board = BOARD_BY_DEVICE_TYPE.get(device_type)
    if not board:
        raise ValueError(f"No board mapping for device type {device_type!r}")
    return board

def _west_sysbuild(board, build_dir, app_dir, extra_args=None):
    subprocess.run(
        ["west", "build", "-b", board, "-d", build_dir, "-p", "--sysbuild"]
        + (["--", *extra_args] if extra_args else []),
        cwd=app_dir,
        check=True,
        capture_output=True
    )

def _require_build_output(path, description):
    if not os.path.isfile(path):
        raise RuntimeError(f"Expected {description} not found: {path}")

def build_app_update(device_type, version, app_dir=APP_DIR, build_dir=APP_UPDATE_BUILD_DIR):
    """Build a signed application update image at the given version.

    Temporarily overwrites the app's VERSION file so the build's
    APP_VERSION_STRING (what the device reports to nRF Cloud) matches
    ``version`` exactly, then restores the original VERSION file. Builds into
    a dedicated directory so the shared dev build (used to flash the baseline
    image) is left untouched.

    :param device_type: DUT_DEVICE_TYPE value (e.g. "thingy91x", "nrf9151dk")
    :param version: MAJOR.MINOR.PATCH string, e.g. from next_app_version()
    :return: Path to the resulting zephyr.signed.bin
    """
    board = _resolve_board(device_type)

    parts = version.split(".")
    if len(parts) != 3 or not all(part.isdigit() for part in parts):
        raise ValueError(f"Invalid version {version!r} (expected MAJOR.MINOR.PATCH)")
    major, minor, patch = parts

    version_path = os.path.join(app_dir, "VERSION")
    with open(version_path) as f:
        original_version_file = f.read()

    logger.info(f"Building app update image at version {version} (board={board})")
    try:
        with open(version_path, "w") as f:
            f.write(
                f"VERSION_MAJOR = {major}\n"
                f"VERSION_MINOR = {minor}\n"
                f"PATCHLEVEL = {patch}\n"
                f"VERSION_TWEAK = 0\n"
                f"EXTRAVERSION =\n"
            )
        _west_sysbuild(board, build_dir, app_dir)
    finally:
        with open(version_path, "w") as f:
            f.write(original_version_file)

    signed_bin = os.path.join(build_dir, "app", "zephyr", "zephyr.signed.bin")
    _require_build_output(signed_bin, "signed update binary")
    return signed_bin

def build_modem_fota_baseline(device_type, app_dir=APP_DIR, build_dir=MODEM_FOTA_BUILD_DIR):
    """Build the baseline app+bootloader image used by the delta modem FOTA test.

    prj.conf enables CONFIG_MEMFAULT_FOTA_MODEM_UPDATE, which requires a real
    CONFIG_MEMFAULT_FOTA_MODEM_PROJECT_KEY at compile time or the device can
    never poll for/apply a modem FOTA job. That key is read from the
    MEMFAULT_FOTA_MODEM_PROJECT_KEY environment variable and compiled in here,
    into a dedicated build directory so the shared dev build is untouched.

    :param device_type: DUT_DEVICE_TYPE value (e.g. "thingy91x", "nrf9151dk")
    :return: Path to the resulting merged.hex
    """
    board = _resolve_board(device_type)

    project_key = os.getenv(MEMFAULT_FOTA_MODEM_PROJECT_KEY_ENV)
    if not project_key:
        raise RuntimeError(
            f"{MEMFAULT_FOTA_MODEM_PROJECT_KEY_ENV} environment variable not set; "
            "required to build a device image that can poll for delta modem FOTA "
            "(find it under Settings -> General in your modem Memfault project)"
        )

    logger.info(f"Building modem FOTA baseline image (board={board})")
    _west_sysbuild(
        board, build_dir, app_dir,
        extra_args=[f'-Dapp_CONFIG_MEMFAULT_FOTA_MODEM_PROJECT_KEY="{project_key}"'],
    )

    merged_hex = os.path.join(build_dir, "merged.hex")
    _require_build_output(merged_hex, "merged hex")
    return merged_hex
