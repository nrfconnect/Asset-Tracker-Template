##########################################################################################
# Copyright (c) 2025 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import pytest
import time
import os
import functools
from utils.flash_tools import flash_device, reset_device
from utils.nrfcloud_fota import FWType, NRFCloudFOTAError
import sys
sys.path.append(os.getcwd())
from utils.logger import get_logger

logger = get_logger()

MFW_202_FILEPATH = "artifacts/mfw_nrf91x1_2.0.2.zip"

# Stable version used for testing
TEST_APP_VERSION = "foo"
TEST_APP_BIN = "artifacts/stable_version_jan_2025-update-signed.bin"

DELTA_MFW_BUNDLEID = "MODEM*3471f88e*mfw_nrf91x1_2.0.2-FOTA-TEST"
FULL_MFW_BUNDLEID = "MDM_FULL*124c2b20*mfw_nrf91x1_full_2.0.2"
NEW_MFW_DELTA_VERSION = "mfw_nrf91x1_2.0.2-FOTA-TEST"
MFW_202_VERSION = "mfw_nrf91x1_2.0.2"

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

def get_appversion(t91x_fota):
    shadow = t91x_fota.fota.get_device(t91x_fota.device_id)
    return shadow["state"]["reported"]["device"]["deviceInfo"]["appVersion"]

def get_modemversion(t91x_fota):
    shadow = t91x_fota.fota.get_device(t91x_fota.device_id)
    return shadow["state"]["reported"]["device"]["deviceInfo"]["modemFirmware"]

def run_fota_resumption(t91x_fota, fota_type):
    timeout_50_percent= APP_FOTA_TIMEOUT/2
    t91x_fota.uart.wait_for_str("50%", timeout=timeout_50_percent)
    logger.debug(f"Testing fota resumption on disconnect for {fota_type} fota")

    patterns_lte_offline = ["network: Network connectivity lost"]
    patterns_lte_normal = ["network: Network connectivity established"]

    # LTE disconnect
    t91x_fota.uart.flush()
    t91x_fota.uart.write("lte offline\r\n")
    t91x_fota.uart.wait_for_str(patterns_lte_offline, timeout=20)

    # LTE reconnect
    t91x_fota.uart.flush()
    t91x_fota.uart.write("lte normal\r\n")
    t91x_fota.uart.wait_for_str(patterns_lte_normal, timeout=120)

    t91x_fota.uart.wait_for_str("fota_download: Refuse fragment, restart with offset")
    t91x_fota.uart.wait_for_str("fota_download: Downloading from offset:")

@pytest.fixture
def run_fota_fixture(t91x_fota, hex_file):
    def _run_fota(bundle_id="", fota_type="app", fotatimeout=APP_FOTA_TIMEOUT, new_version=TEST_APP_VERSION):
        flash_device(os.path.abspath(hex_file))
        t91x_fota.uart.xfactoryreset()
        t91x_fota.uart.flush()
        reset_device()
        t91x_fota.uart.wait_for_str("Connected to Cloud")

        if fota_type == "app":
            bundle_id = t91x_fota.fota.upload_firmware(
                "nightly_test_app",
                TEST_APP_BIN,
                TEST_APP_VERSION,
                "Bundle used for nightly test",
                FWType.app,
            )
            logger.info(f"Uploaded file {TEST_APP_BIN}: bundleId: {bundle_id}")

        try:
            t91x_fota.data['job_id'] = t91x_fota.fota.create_fota_job(t91x_fota.device_id, bundle_id)
            t91x_fota.data['bundle_id'] = bundle_id
        except NRFCloudFOTAError as e:
            pytest.skip(f"FOTA create_job REST API error: {e}")
        logger.info(f"Created FOTA Job (ID: {t91x_fota.data['job_id']})")

        # Sleep a bit and trigger fota poll
        for i in range(3):
            try:
                time.sleep(30)
                t91x_fota.uart.write("zbus button_press\r\n")
                t91x_fota.uart.wait_for_str("nrf_cloud_fota_poll: Starting FOTA download")
                break
            except AssertionError:
                continue
        else:
            raise AssertionError(f"Fota update not available after {i} attempts")


        if fota_type == "app":
            run_fota_resumption(t91x_fota, "app")

        await_nrfcloud(
                functools.partial(t91x_fota.fota.get_fota_status, t91x_fota.data['job_id']),
                "COMPLETED",
                "FOTA status",
                fotatimeout
            )
        try:
            if fota_type == "app":
                await_nrfcloud(
                    functools.partial(get_appversion, t91x_fota),
                    new_version,
                    "appVersion",
                    DEVICE_MSG_TIMEOUT
                )
            else:
                await_nrfcloud(
                    functools.partial(get_modemversion, t91x_fota),
                    new_version,
                    "modemFirmware",
                    DEVICE_MSG_TIMEOUT
                )
        except RuntimeError:
            logger.error(f"Version is not {new_version} after {DEVICE_MSG_TIMEOUT}s")

    return _run_fota


def test_app_fota(run_fota_fixture):
    '''
    Test application FOTA from nightly version to stable version
    '''
    run_fota_fixture()  # Uses default parameters for app FOTA

def test_delta_mfw_fota(run_fota_fixture):
    '''
    Test delta modem FOTA on nrf9151
    '''
    try:
        run_fota_fixture(
            bundle_id=DELTA_MFW_BUNDLEID,
            fota_type="delta",
            new_version=NEW_MFW_DELTA_VERSION
        )
    finally:
        # Restore mfw202, no matter if test pass/fails
        flash_device(os.path.abspath(MFW_202_FILEPATH))

@pytest.mark.slow
def test_full_mfw_fota(run_fota_fixture):
    '''
    Test full modem FOTA on nrf9151
    '''
    run_fota_fixture(
        bundle_id=FULL_MFW_BUNDLEID,
        fota_type="full",
        new_version=MFW_202_VERSION,
        fotatimeout=FULL_MFW_FOTA_TIMEOUT
    )
