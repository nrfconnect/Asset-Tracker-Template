##########################################################################################
# Copyright (c) 2025 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import pytest
import time
import os
import functools
from utils.flash_tools import flash_device, reset_device, next_app_version
from utils.nrfcloud import NRFCloudFOTAError
import sys
sys.path.append(os.getcwd())
from utils.logger import get_logger

logger = get_logger()

MFW_SOFTWARE_TYPE = "mfw"
MFW_BASE_VERSION = "mfw_nrf91x1_2.0.3"
MFW_TARGET_VERSION = "mfw_nrf91x1_2.0.4"

# Memfault applies SemVer precedence when deciding whether a delta represents
# an upgrade: a version with a pre-release suffix (e.g. "2.0.4-FOTA-TEST") has
# *lower* precedence than the plain version ("2.0.4"), per
# https://semver.org/#spec-item-11, so a device already at "2.0.4" is never
# offered the "2.0.4-FOTA-TEST" delta. Using the real 2.0.3 -> 2.0.4 delta
# (published inside Nordic's mfw_nrf91x1_2.0.4 release zip) avoids this: the
# device is first flashed down to the full 2.0.3 baseline directly, then
# FOTA'd up to 2.0.4 via Memfault, which is an unambiguous upgrade.
MFW_BASELINE_ZIP = os.getenv("MFW_BASELINE_ZIP", "artifacts/mfw_nrf91x1_2.0.3.zip")
MFW_TARGET_ZIP = os.getenv("MFW_TARGET_ZIP", "artifacts/mfw_nrf91x1_2.0.4.zip")
DELTA_MFW_BIN_203_TO_204 = os.getenv(
    "DELTA_MFW_BIN_203_TO_204", "artifacts/mfw_nrf91x1_update_from_2.0.3_to_2.0.4.bin")

DEVICE_MSG_TIMEOUT = 60 * 7
APP_FOTA_TIMEOUT = 60 * 15

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

def deactivate_release(ota, cohort, software_version):
    """Best-effort deactivation of a release from a cohort."""
    try:
        ota.deactivate_release(cohort, software_version)
    except NRFCloudFOTAError as e:
        logger.warning(f"Failed to deactivate release {software_version}: {e}. Note this is expected if the release does not exist yet.")

def restore_device_after_modem_fota(dut_fota, hex_file):
    """Return the DUT to a known-good state after modem FOTA tests."""
    logger.info("Restoring device after modem FOTA test")

    deactivate_release(dut_fota.modem_ota, dut_fota.modem_cohort, MFW_TARGET_VERSION)
    flash_device(os.path.abspath(MFW_TARGET_ZIP))

    # Reflash the application image to ensure the device is back to a known-good state
    flash_device(os.path.abspath(hex_file))

    try:
        dut_fota.uart.xfactoryreset()
        dut_fota.uart.flush()
    except Exception as e:
        logger.warning(f"Factory reset during restore failed: {e}")

    reset_device()

def trigger_fota_poll(dut_fota, max_attempts=6):
    # 6 attempts * (10s sleep + 30s wait) = 4 minutes total, to allow for
    # propagation delay after a release is deployed on the Memfault backend.
    for _ in range(max_attempts):
        try:
            time.sleep(10)
            dut_fota.uart.write("att_fota poll\r\n")
            dut_fota.uart.wait_for_str("nrf_cloud_fota_poll: Starting FOTA download", timeout=30)
            return
        except AssertionError:
            continue
    raise AssertionError(f"Fota update not available after {max_attempts} attempts")

@pytest.fixture
def run_fota_fixture(dut_fota, hex_file):
    def _run_fota(bin_path, new_version, fota_type="app", initial_hex_file=None, delta_from=None):
        flash_device(os.path.abspath(initial_hex_file or hex_file))
        dut_fota.uart.xfactoryreset()
        dut_fota.uart.flush()
        reset_device()

        dut_fota.uart.wait_for_str_with_retries("Connected to Cloud", max_retries=3, timeout=240, reset_func=reset_device)

        fota_targets = {
            "app": (dut_fota.fota, dut_fota.cohort, dut_fota.hw_version, dut_fota.app_software_type),
            "delta": (dut_fota.modem_ota, dut_fota.modem_cohort, dut_fota.modem_hw_version, MFW_SOFTWARE_TYPE),
        }
        ota, cohort, hardware_version, software_type = fota_targets[fota_type]

        try:
            ota.upload_ota_payload(
                bin_path=os.path.abspath(bin_path),
                hardware_version=hardware_version,
                software_type=software_type,
                software_version=None if delta_from else new_version,
                delta_from=delta_from,
                delta_to=new_version if delta_from else None,
            )
            ota.deploy_release(
                cohort,
                software_version=None if delta_from else new_version,
                delta_from=delta_from,
                delta_to=new_version if delta_from else None,
            )
        except NRFCloudFOTAError as e:
            pytest.skip(f"OTA deploy error: {e}")

        trigger_fota_poll(dut_fota)

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

    return _run_fota


@pytest.mark.slow
def test_app_fota(run_fota_fixture, dut_fota, app_fota_update_bin_file):
    '''
    Test application FOTA from the currently-flashed build to a freshly built
    image at a version guaranteed greater than the current app version,
    delivered through nRF Cloud's OTA system. The update image is built by CI
    (see build.yml's app-fota-update steps) with PATCHLEVEL+1, not at test time.
    '''
    new_version = next_app_version()

    # Deactivate the release about to be performed in case it is active
    deactivate_release(dut_fota.fota, dut_fota.cohort, new_version)

    try:
        run_fota_fixture(
            bin_path=app_fota_update_bin_file,
            new_version=new_version,
        )
    finally:
        # Deactivate the release to clean up after the test in case it is still active
        deactivate_release(dut_fota.fota, dut_fota.cohort, new_version)

@pytest.mark.slow
def test_delta_mfw_fota(dut_fota, run_fota_fixture, modem_fota_baseline_hex_file):
    '''
    Test delta modem FOTA on nrf9151, delivered through nRF Cloud's OTA system.

    Flashes the modem down to the full 2.0.3 baseline directly, then flashes a
    dedicated app image with CONFIG_MEMFAULT_FOTA_MODEM_PROJECT_KEY compiled in
    (built by CI, see build.yml's modem-fota-baseline steps) -- without it the
    device can never poll for a modem FOTA job -- then FOTAs the modem up to
    2.0.4 via Memfault OTA, then restores back to 2.0.4 by flashing the full
    baseline directly (see restore_device_after_modem_fota).
    '''
    if not dut_fota.modem_ota:
        pytest.skip("Modem OTA project not configured (MEMFAULT_MODEM_PROJECT_SLUG)")

    modem_fota_hex = modem_fota_baseline_hex_file
    flash_device(os.path.abspath(MFW_BASELINE_ZIP))

    # The generic FOTA poll checks the app cohort before the modem cohort (see
    # memfault_nrf_cloud_fota_override.c), so a release left active there would
    # cause the device to pick up an app update instead of the modem one below.
    deactivate_release(dut_fota.fota, dut_fota.cohort, next_app_version())

    # Deactivate the release about to be performed in case it is active
    deactivate_release(dut_fota.modem_ota, dut_fota.modem_cohort, MFW_TARGET_VERSION)

    try:
        run_fota_fixture(
            bin_path=DELTA_MFW_BIN_203_TO_204,
            fota_type="delta",
            new_version=MFW_TARGET_VERSION,
            initial_hex_file=modem_fota_hex,
            delta_from=MFW_BASE_VERSION,
        )
    finally:
        restore_device_after_modem_fota(dut_fota, modem_fota_hex)
