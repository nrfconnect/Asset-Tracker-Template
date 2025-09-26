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

MFW_202_FILEPATH = "artifacts/mfw_nrf91x1_2.0.2.zip"

# Stable version used for testing
TEST_APP_VERSION = "1.0.2"

DELTA_MFW_BUNDLEID_20X_TO_FOTA_TEST = "59cec896-c842-40fe-9a95-a4f3e88a4cdb"
DELTA_MFW_BUNDLEID_FOTA_TEST_TO_20X = "7b79d95d-5f3b-4ae1-9a04-214ec273515d"
FULL_MFW_BUNDLEID = "d692915d-d978-4c77-ab02-f05f511971f9"
MFW_DELTA_VERSION_20X_FOTA_TEST = "mfw_nrf91x1_2.0.2-FOTA-TEST"
MFW_202_VERSION = "mfw_nrf91x1_2.0.2"

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

def run_fota_resumption(dut_fota, fota_type):
    timeout_50_percent= APP_FOTA_TIMEOUT/2
    dut_fota.uart.wait_for_str("50%", timeout=timeout_50_percent)
    logger.debug(f"Testing fota resumption on disconnect for {fota_type} fota")

    patterns_lte_offline = ["network: Network connectivity lost"]
    patterns_lte_normal = ["network: Network connectivity established"]

    # LTE disconnect
    dut_fota.uart.flush()
    dut_fota.uart.write("att_network disconnect\r\n")
    dut_fota.uart.wait_for_str(patterns_lte_offline, timeout=20)

    # LTE reconnect
    dut_fota.uart.flush()
    dut_fota.uart.write("att_network connect\r\n")
    dut_fota.uart.wait_for_str(patterns_lte_normal, timeout=120)
    dut_fota.uart.wait_for_str("fota_download: Refuse fragment, restart with offset")
    dut_fota.uart.wait_for_str("fota_download: Downloading from offset:")

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
                            MFW_202_VERSION,
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
            new_version=MFW_DELTA_VERSION_20X_FOTA_TEST
        )
    finally:
        # Restore mfw202, no matter if test pass/fails
        flash_device(os.path.abspath(MFW_202_FILEPATH))

@pytest.mark.slow
def test_full_mfw_fota(run_fota_fixture):
    '''
    Test full modem FOTA on nrf9151
    '''

    try:
        run_fota_fixture(
            bundle_id=FULL_MFW_BUNDLEID,
            fota_type="full",
            new_version=MFW_202_VERSION,
            fotatimeout=FULL_MFW_FOTA_TIMEOUT,
            reschedule=True
        )
    finally:
        # Restore mfw202, no matter if test pass/fails
        flash_device(os.path.abspath(MFW_202_FILEPATH))
