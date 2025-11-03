##########################################################################################
# Copyright (c) 2025 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import pytest
import time
import os
import functools
from utils.flash_tools import flash_device, reset_device
from utils.nrfcloud import NRFCloudFOTAError
import sys
sys.path.append(os.getcwd())
from utils.logger import get_logger

logger = get_logger()

MFW_FILEPATH = "artifacts/mfw_nrf91x1_2.0.3.zip"

# Stable version used for testing
TEST_APP_VERSION = "1.0.2"

DELTA_MFW_BUNDLEID_20X_TO_FOTA_TEST = "c8443b86-d295-4305-8e39-0ae2731178e1"
DELTA_MFW_BUNDLEID_FOTA_TEST_TO_20X = "fcefc1f6-ee51-469a-9c06-8237a42acf95"
FULL_MFW_BUNDLEID = "61cedb8d-0b6f-4684-9150-5aa782c6c8d5"
MFW_DELTA_VERSION_FOTA_TEST = "mfw_nrf91x1_2.0.3-FOTA-TEST"
MFW_VERSION = "mfw_nrf91x1_2.0.3"

APP_BUNDLEID = os.getenv("APP_BUNDLEID")

TEST_APP_BIN = {
    "thingy91x": "artifacts/stable_version_jan_2025-update-signed.bin",
    "nrf9151dk": "artifacts/nrf9151dk_mar_2025_update_signed.bin"
}

DEVICE_MSG_TIMEOUT = 60 * 5
APP_FOTA_TIMEOUT = 60 * 10
FULL_MFW_FOTA_TIMEOUT = 60 * 30

def await_nrfcloud(func, expected, field, timeout):
    start = time.time()
    logger.info(f"Awaiting {field} == {expected} in nrfcloud shadow...")
    while True:
        time.sleep(5)
        if time.time() - start > timeout:
            raise RuntimeError(f"Timeout awaiting {field} update")
        try:
            data = func()
        except Exception as e:
            logger.warning(f"Exception {e} during waiting for {field}")
            continue
        logger.debug(f"Reported {field}: {data}")
        if expected in data:
            break

def get_appversion(dut_fota):
    shadow = dut_fota.fota.get_device(dut_fota.device_id)
    return shadow["state"]["reported"]["device"]["deviceInfo"]["appVersion"]

def get_modemversion(dut_fota):
    shadow = dut_fota.fota.get_device(dut_fota.device_id)
    return shadow["state"]["reported"]["device"]["deviceInfo"]["modemFirmware"]

def perform_disconnect_reconnect(dut_fota, expected_percentage):
    """Helper function to perform a disconnect/reconnect sequence and verify resumption at expected percentage"""
    patterns_lte_offline = ["network: l4_event_handler: Network connectivity lost"]
    patterns_lte_normal = ["network: l4_event_handler: Network connectivity established"]

    logger.info(f"Disconnecting at {expected_percentage}% - device should resume at same percentage")

    # LTE disconnect
    dut_fota.uart.flush()
    dut_fota.uart.write("att_network disconnect\r\n")
    dut_fota.uart.wait_for_str(patterns_lte_offline, timeout=20)

    # LTE reconnect
    dut_fota.uart.flush()
    dut_fota.uart.write("att_network connect\r\n")
    dut_fota.uart.wait_for_str(patterns_lte_normal, timeout=120)
    dut_fota.uart.wait_for_str("fota_download: Refuse fragment, restart with offset", timeout=600)
    dut_fota.uart.wait_for_str("fota_download: Downloading from offset:", timeout=600)

    # Verify resumption starts at or very close to the expected percentage
    # Look for the next percentage update to confirm we're resuming properly
    next_percentage = expected_percentage + 5
    try:
        # Wait for the next percentage (or same percentage if we're exactly at boundary)
        dut_fota.uart.wait_for_str(f"{expected_percentage}%", timeout=60)
        logger.info(f"✓ Verified: Resumed at {expected_percentage}% as expected")
    except AssertionError:
        try:
            # If we don't see the exact percentage, look for the next one
            dut_fota.uart.wait_for_str(f"{next_percentage}%", timeout=60)
            logger.info(f"✓ Verified: Resumed correctly, now at {next_percentage}%")
        except AssertionError:
            logger.error(f"✗ Failed to verify resumption at expected percentage {expected_percentage}%")
            raise AssertionError(f"Could not verify FOTA resumed at {expected_percentage}%")

def run_fota_resumption(dut_fota, fota_type):
    if fota_type == "app":
        timeout_50_percent = APP_FOTA_TIMEOUT/2
        dut_fota.uart.wait_for_str("50%", timeout=timeout_50_percent)
        logger.debug(f"Testing fota resumption on disconnect for {fota_type} fota")

        perform_disconnect_reconnect(dut_fota, 50)
    elif fota_type == "full":
        # Test resumption at 20% and 80%
        logger.debug(f"Testing fota resumption on disconnect for {fota_type} fota at 20% and 80%")

        # First disconnect at 20%
        timeout_20_percent = FULL_MFW_FOTA_TIMEOUT * 0.2
        dut_fota.uart.wait_for_str("20%", timeout=timeout_20_percent)
        logger.info("Performing first disconnect/reconnect at 20%")
        perform_disconnect_reconnect(dut_fota, 20)

        # Second disconnect at 80%
        timeout_80_percent = FULL_MFW_FOTA_TIMEOUT * 0.6  # Additional 60% of total timeout
        dut_fota.uart.wait_for_str("80%", timeout=timeout_80_percent)
        logger.info("Performing second disconnect/reconnect at 80%")
        perform_disconnect_reconnect(dut_fota, 80)

def run_fota_reschedule(dut_fota, fota_type):
    dut_fota.uart.wait_for_str("5%", timeout=APP_FOTA_TIMEOUT)
    logger.debug(f"Cancelling FOTA, type: {fota_type}")

    dut_fota.fota.cancel_fota_job(dut_fota.data['job_id'])

    await_nrfcloud(
        functools.partial(dut_fota.fota.get_fota_status, dut_fota.data['job_id']),
        "CANCELLED",
        "FOTA status",
        APP_FOTA_TIMEOUT
    )

    patterns_fota_cancel = ["Firmware download canceled", "state_waiting_for_poll_request_entry"]

    dut_fota.uart.wait_for_str(patterns_fota_cancel, timeout=180)

    dut_fota.data['job_id'] = dut_fota.fota.create_fota_job(dut_fota.device_id, dut_fota.data['bundle_id'])

    logger.info(f"Rescheduled FOTA Job (ID: {dut_fota.data['job_id']})")

    # Sleep a bit and trigger fota poll
    for i in range(3):
        try:
            time.sleep(30)
            dut_fota.uart.write("att_button_press 1\r\n")
            dut_fota.uart.wait_for_str("nrf_cloud_fota_poll: Starting FOTA download")
            break
        except AssertionError:
            continue
    else:
        raise AssertionError(f"Fota update not available after {i} attempts")

@pytest.fixture
def run_fota_fixture(dut_fota, hex_file, reschedule=False):
    def _run_fota(bundle_id="", fota_type="app", fotatimeout=APP_FOTA_TIMEOUT, new_version=TEST_APP_VERSION, reschedule=False):
        flash_device(os.path.abspath(hex_file))
        dut_fota.uart.xfactoryreset()
        dut_fota.uart.flush()
        reset_device()

        dut_fota.uart.wait_for_str_with_retries("Connected to Cloud", max_retries=3, timeout=240, reset_func=reset_device)


        try:
            dut_fota.data['job_id'] = dut_fota.fota.create_fota_job(dut_fota.device_id, bundle_id)
            dut_fota.data['bundle_id'] = bundle_id
        except NRFCloudFOTAError as e:
            pytest.skip(f"FOTA create_job REST API error: {e}")
        logger.info(f"Created FOTA Job (ID: {dut_fota.data['job_id']})")

        # Sleep a bit and trigger fota poll
        for i in range(3):
            try:
                time.sleep(10)
                dut_fota.uart.write("att_button_press 1\r\n")
                dut_fota.uart.wait_for_str("nrf_cloud_fota_poll: Starting FOTA download", timeout=30)
                break
            except AssertionError:
                continue
        else:
            raise AssertionError(f"Fota update not available after {i} attempts")

        if reschedule:
            run_fota_reschedule(dut_fota, fota_type)

        if fota_type == "app":
            run_fota_resumption(dut_fota, "app")
        elif fota_type == "full":
            run_fota_resumption(dut_fota, "full")
        await_nrfcloud(
            functools.partial(dut_fota.fota.get_fota_status, dut_fota.data['job_id']),
            "IN_PROGRESS",
            "FOTA status",
            fotatimeout
        )
        await_nrfcloud(
            functools.partial(dut_fota.fota.get_fota_status, dut_fota.data['job_id']),
            "COMPLETED",
            "FOTA status",
            fotatimeout
        )

        try:
            if fota_type == "app":
                await_nrfcloud(
                    functools.partial(get_appversion, dut_fota),
                    new_version,
                    "appVersion",
                    DEVICE_MSG_TIMEOUT
                )
            else:
                await_nrfcloud(
                    functools.partial(get_modemversion, dut_fota),
                    new_version,
                    "modemFirmware",
                    DEVICE_MSG_TIMEOUT
                )
        except RuntimeError as e:
            logger.error(f"Version is not {new_version} after {DEVICE_MSG_TIMEOUT}s")
            raise e

        if fota_type == "delta":
            # Run a second delta fota back from FOTA-TEST
            logger.info("Running a second delta fota back from FOTA-TEST")
            try:
                dut_fota.data['job_id'] = dut_fota.fota.create_fota_job(dut_fota.device_id, DELTA_MFW_BUNDLEID_FOTA_TEST_TO_20X)
                dut_fota.data['bundle_id'] = bundle_id
            except NRFCloudFOTAError as e:
                pytest.skip(f"FOTA create_job REST API error: {e}")
            logger.info(f"Created FOTA Job (ID: {dut_fota.data['job_id']})")

            # Sleep a bit and trigger fota poll
            dut_fota.uart.flush()
            for i in range(3):
                try:
                    time.sleep(10)
                    dut_fota.uart.write("att_button_press 1\r\n")
                    dut_fota.uart.wait_for_str("nrf_cloud_fota_poll: Starting FOTA download", timeout=30)
                    break
                except AssertionError:
                    continue
            else:
                raise AssertionError(f"Fota update not available after {i} attempts")

            await_nrfcloud(
                functools.partial(dut_fota.fota.get_fota_status, dut_fota.data['job_id']),
                "IN_PROGRESS",
                "FOTA status",
                fotatimeout
            )
            await_nrfcloud(
                functools.partial(dut_fota.fota.get_fota_status, dut_fota.data['job_id']),
                "COMPLETED",
                "FOTA status",
                fotatimeout
            )

            try:
                await_nrfcloud(
                    functools.partial(get_modemversion, dut_fota),
                    MFW_VERSION,
                    "modemFirmware",
                    DEVICE_MSG_TIMEOUT
                )
            except RuntimeError as e:
                logger.error(f"Version is not {new_version} after {DEVICE_MSG_TIMEOUT}s")
                raise e

    return _run_fota


@pytest.mark.slow
def test_app_fota(run_fota_fixture):
    '''
    Test application FOTA from nightly version to stable version
    '''
    run_fota_fixture(
        bundle_id=APP_BUNDLEID,
    )

def test_delta_mfw_fota(run_fota_fixture):
    '''
    Test delta modem FOTA on nrf9151
    '''
    try:
        run_fota_fixture(
            bundle_id=DELTA_MFW_BUNDLEID_20X_TO_FOTA_TEST,
            fota_type="delta",
            new_version=MFW_DELTA_VERSION_FOTA_TEST
        )
    finally:
        # Restore mfw, no matter if test pass/fails
        flash_device(os.path.abspath(MFW_FILEPATH))

@pytest.mark.slow
def test_full_mfw_fota(run_fota_fixture):
    '''
    Test full modem FOTA on nrf9151
    '''

    try:
        run_fota_fixture(
            bundle_id=FULL_MFW_BUNDLEID,
            fota_type="full",
            new_version=MFW_VERSION,
            fotatimeout=FULL_MFW_FOTA_TIMEOUT,
            reschedule=True
        )
    finally:
        # Restore mfw, no matter if test pass/fails
        flash_device(os.path.abspath(MFW_FILEPATH))
