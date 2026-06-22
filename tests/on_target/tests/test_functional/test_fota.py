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

MFW_FILEPATH = "artifacts/mfw_nrf91x1_2.0.4.zip"

# Stable version used for testing
TEST_APP_VERSION = "1.0.2"

DELTA_MFW_BUNDLEID_20X_TO_FOTA_TEST = "5060efda-fcae-48d1-ab2d-7cfeb7dde8a9"
DELTA_MFW_BUNDLEID_FOTA_TEST_TO_20X = "c1e5d090-1217-47ef-ac4e-b74339c50a06"
FULL_MFW_BUNDLEID = "02fd1b8f-5c06-43e7-8c9c-173a50259456"
MFW_DELTA_VERSION_FOTA_TEST = "mfw_nrf91x1_2.0.4-FOTA-TEST"
MFW_VERSION = "mfw_nrf91x1_2.0.4"

APP_BUNDLEID = os.getenv("APP_BUNDLEID")
MCUBOOT_BUNDLEID = os.getenv("MCUBOOT_BUNDLEID")

BOOTLOADER_VERSION_BASELINE = "2"
BOOTLOADER_VERSION_UPDATED = "3"
BOOTLOADER_FIRMWARE_VERSION_LOG = "Firmware version 3"

FOTA_STATUS_DETAIL_SUCCESS = "FOTA update completed successfully"

TEST_APP_BIN = {
    "thingy91x": "artifacts/stable_version_jan_2025-update-signed.bin",
    "nrf9151dk": "artifacts/nrf9151dk_mar_2025_update_signed.bin"
}

DEVICE_MSG_TIMEOUT = 60 * 5
APP_FOTA_TIMEOUT = 60 * 15
BOOTLOADER_FOTA_TIMEOUT = 60 * 20
FULL_MFW_FOTA_TIMEOUT = 60 * 30

def await_nrfcloud(func, expected, field, timeout, expected_detail=None):
    start = time.time()
    if expected_detail is not None:
        logger.info(
            f"Awaiting {field} == {expected} and "
            f"statusDetail == '{expected_detail}' in nrfcloud..."
        )
    else:
        logger.info(f"Awaiting {field} == {expected} in nrfcloud shadow...")
    while True:
        time.sleep(5)
        if time.time() - start > timeout:
            if expected_detail is not None:
                try:
                    data = func()
                    if isinstance(data, dict):
                        status = data.get("status", "<missing>")
                        status_detail = data.get("statusDetail", "<missing>")
                    else:
                        status = data
                        status_detail = "<unexpected response type>"
                except Exception as e:
                    status = f"<failed to fetch: {e}>"
                    status_detail = status
                raise RuntimeError(
                    f"Timeout awaiting {field} == {expected} with "
                    f"statusDetail == '{expected_detail}'. "
                    f"Got status: {status!r}, statusDetail: {status_detail!r}")
            raise RuntimeError(f"Timeout awaiting {field} update")
        try:
            data = func()
        except Exception as e:
            logger.warning(f"Exception {e} during waiting for {field}")
            continue
        if expected_detail is not None:
            if not isinstance(data, dict):
                logger.warning(
                    f"Expected dict response when checking statusDetail, got {type(data)}")
                continue
            status = data.get("status")
            status_detail = data.get("statusDetail")
            logger.debug(
                f"Reported {field}: status={status!r}, statusDetail={status_detail!r}")
            if status is not None and expected in status:
                if status_detail == expected_detail:
                    break
                raise RuntimeError(
                    f"{field} matched {expected!r} but unexpected statusDetail: "
                    f"{status_detail!r} (expected {expected_detail!r})")
        else:
            logger.debug(f"Reported {field}: {data}")
            if expected in data:
                break

def await_fota_job_succeeded(dut_fota, job_id, timeout):
    """Wait for FOTA job to complete and the device execution to succeed."""
    await_nrfcloud(
        functools.partial(dut_fota.fota.get_fota_status, job_id),
        "IN_PROGRESS",
        "FOTA status",
        timeout
    )
    await_nrfcloud(
        functools.partial(dut_fota.fota.get_fota_status, job_id),
        "COMPLETED",
        "FOTA status",
        timeout
    )
    await_nrfcloud(
        functools.partial(dut_fota.fota.get_fota_execution, dut_fota.device_id, job_id),
        "SUCCEEDED",
        "FOTA execution status",
        timeout,
        expected_detail=FOTA_STATUS_DETAIL_SUCCESS,
    )

def get_appversion(dut_fota):
    shadow = dut_fota.fota.get_device(dut_fota.device_id)
    return shadow["state"]["reported"]["device"]["deviceInfo"]["appVersion"]

def get_modemversion(dut_fota):
    shadow = dut_fota.fota.get_device(dut_fota.device_id)
    return shadow["state"]["reported"]["device"]["deviceInfo"]["modemFirmware"]

def get_bootloaderversion(dut_fota):
    shadow = dut_fota.fota.get_device(dut_fota.device_id)
    return shadow["state"]["reported"]["device"]["deviceInfo"]["bootloaderVersion"]

def await_bootloader_version(dut_fota, expected, timeout=DEVICE_MSG_TIMEOUT):
    start = time.time()
    logger.info(f"Awaiting bootloaderVersion == {expected} in nrfcloud shadow...")
    while True:
        time.sleep(5)
        if time.time() - start > timeout:
            raise RuntimeError(
                f"Timeout awaiting bootloaderVersion == {expected}")
        try:
            version = get_bootloaderversion(dut_fota)
        except (KeyError, TypeError) as e:
            logger.warning(f"bootloaderVersion not in shadow yet: {e}")
            continue
        except Exception as e:
            logger.warning(f"Exception getting bootloaderVersion: {e}")
            continue
        logger.debug(f"Reported bootloaderVersion: {version}")
        if version == expected:
            return

def restore_device_after_modem_fota(dut_fota, hex_file):
    """Return the DUT to a known-good state after modem FOTA tests."""
    logger.info("Restoring device after modem FOTA test")

    job_id = dut_fota.data.get("job_id")
    if job_id:
        try:
            dut_fota.fota.cancel_fota_job(job_id)
        except Exception as e:
            logger.warning(f"Failed to cancel active FOTA job {job_id}: {e}")

    try:
        dut_fota.fota.ensure_no_pending_fota_jobs(dut_fota.device_id)
    except Exception as e:
        logger.warning(f"Failed to cancel pending FOTA jobs during restore: {e}")

    flash_device(os.path.abspath(MFW_FILEPATH))
    flash_device(os.path.abspath(hex_file))

    try:
        dut_fota.uart.xfactoryreset()
        dut_fota.uart.flush()
    except Exception as e:
        logger.warning(f"Factory reset during restore failed: {e}")

    reset_device()

    try:
        dut_fota.fota.ensure_no_pending_fota_jobs(dut_fota.device_id)
    except Exception as e:
        logger.warning(f"Failed to cancel pending FOTA jobs after restore: {e}")

def trigger_fota_poll(dut_fota, max_attempts=3):
    for _ in range(max_attempts):
        try:
            time.sleep(10)
            dut_fota.uart.write("att_fota poll\r\n")
            dut_fota.uart.wait_for_str("nrf_cloud_fota_poll: Starting FOTA download", timeout=30)
            return
        except AssertionError:
            continue
    raise AssertionError(f"Fota update not available after {max_attempts} attempts")

def perform_disconnect_reconnect(dut_fota, expected_percentage):
    """Helper function to perform a disconnect/reconnect sequence and verify resumption at expected percentage"""
    patterns_lte_offline = ["network: lte_lc_evt_handler: PDN connection network detached"]
    patterns_lte_normal = ["network: lte_lc_evt_handler: PDN connection activated"]

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
        logger.info(f"Performing first disconnect/reconnect at 20%")
        perform_disconnect_reconnect(dut_fota, 20)

        # Second disconnect at 80%
        timeout_80_percent = FULL_MFW_FOTA_TIMEOUT * 0.6  # Additional 60% of total timeout
        dut_fota.uart.wait_for_str("80%", timeout=timeout_80_percent)
        logger.info(f"Performing second disconnect/reconnect at 80%")
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
            dut_fota.uart.write("att_fota poll\r\n")
            dut_fota.uart.wait_for_str("nrf_cloud_fota_poll: Starting FOTA download")
            break
        except AssertionError:
            continue
    else:
        raise AssertionError(f"Fota update not available after {i} attempts")

@pytest.fixture(autouse=True)
def ensure_no_pending_fota_jobs_before_test(dut_fota):
    """Ensure the DUT has no pending FOTA jobs before each test."""
    dut_fota.fota.ensure_no_pending_fota_jobs(dut_fota.device_id)

@pytest.fixture
def run_fota_fixture(dut_fota, hex_file, reschedule=False):
    def _run_fota(bundle_id="", fota_type="app", fotatimeout=APP_FOTA_TIMEOUT, new_version=TEST_APP_VERSION, reschedule=False):
        flash_device(os.path.abspath(hex_file))
        dut_fota.uart.xfactoryreset()
        dut_fota.uart.flush()
        reset_device()

        dut_fota.uart.wait_for_str_with_retries("Connected to Cloud", max_retries=3, timeout=240, reset_func=reset_device)

        dut_fota.fota.ensure_no_pending_fota_jobs(dut_fota.device_id)

        try:
            dut_fota.data['job_id'] = dut_fota.fota.create_fota_job(dut_fota.device_id, bundle_id)
            dut_fota.data['bundle_id'] = bundle_id
        except NRFCloudFOTAError as e:
            pytest.skip(f"FOTA create_job REST API error: {e}")
        logger.info(f"Created FOTA Job (ID: {dut_fota.data['job_id']})")

        trigger_fota_poll(dut_fota)

        if reschedule:
            run_fota_reschedule(dut_fota, fota_type)

        if fota_type == "app":
            run_fota_resumption(dut_fota, "app")
        elif fota_type == "full":
            run_fota_resumption(dut_fota, "full")
        await_fota_job_succeeded(dut_fota, dut_fota.data['job_id'], fotatimeout)

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
                    dut_fota.uart.write("att_fota poll\r\n")
                    dut_fota.uart.wait_for_str("nrf_cloud_fota_poll: Starting FOTA download", timeout=30)
                    break
                except AssertionError:
                    continue
            else:
                raise AssertionError(f"Fota update not available after {i} attempts")

            await_fota_job_succeeded(dut_fota, dut_fota.data['job_id'], fotatimeout)

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

@pytest.mark.slow
def test_bootloader_fota(dut_fota, hex_file):
    '''
    Test MCUboot bootloader (B1) FOTA
    '''
    if not MCUBOOT_BUNDLEID:
        pytest.skip("MCUBOOT_BUNDLEID environment variable not set")

    try:
        flash_device(os.path.abspath(hex_file))
        dut_fota.uart.xfactoryreset()
        dut_fota.uart.flush()
        reset_device()

        dut_fota.uart.wait_for_str_with_retries(
            "Connected to Cloud", max_retries=3, timeout=240, reset_func=reset_device)

        await_bootloader_version(dut_fota, BOOTLOADER_VERSION_BASELINE)

        dut_fota.fota.ensure_no_pending_fota_jobs(dut_fota.device_id)

        try:
            dut_fota.data["job_id"] = dut_fota.fota.create_fota_job(
                dut_fota.device_id, MCUBOOT_BUNDLEID)
            dut_fota.data["bundle_id"] = MCUBOOT_BUNDLEID
        except NRFCloudFOTAError as e:
            pytest.skip(f"FOTA create_job REST API error: {e}")
        logger.info(f"Created bootloader FOTA job (ID: {dut_fota.data['job_id']})")

        trigger_fota_poll(dut_fota)

        dut_fota.uart.wait_for_str("fota_download: B1 update, selected", timeout=120)
        dut_fota.uart.wait_for_str("Download complete", timeout=BOOTLOADER_FOTA_TIMEOUT)
        post_download_pos = dut_fota.uart.get_size()

        dut_fota.uart.wait_for_str(
            BOOTLOADER_FIRMWARE_VERSION_LOG,
            timeout=BOOTLOADER_FOTA_TIMEOUT,
            start_pos=post_download_pos,
            error_msg="Expected B0 fw_info v3 after download",
        )

        await_fota_job_succeeded(dut_fota, dut_fota.data["job_id"], BOOTLOADER_FOTA_TIMEOUT)

        dut_fota.uart.wait_for_str_with_retries(
            "Connected to Cloud", max_retries=5, timeout=300, reset_func=reset_device)

        await_bootloader_version(dut_fota, BOOTLOADER_VERSION_UPDATED,
                                 timeout=BOOTLOADER_FOTA_TIMEOUT)
    finally:
        flash_device(os.path.abspath(hex_file))

def test_delta_mfw_fota(dut_fota, run_fota_fixture, hex_file):
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
        restore_device_after_modem_fota(dut_fota, hex_file)

@pytest.mark.slow
def test_full_mfw_fota(dut_fota, run_fota_fixture, hex_file):
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
        restore_device_after_modem_fota(dut_fota, hex_file)
