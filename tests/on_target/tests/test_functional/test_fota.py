##########################################################################################
# Copyright (c) 2025 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

"""FOTA tests of the firmware with Memfault OTA.

The application, the bootloader and the modem firmware are software types of one Memfault
project. All the tests share one release, which has the version of the fota-target build (the
application version that sorts after the baseline build). A test puts its images in the release,
deploys it to the cohort of the DUT, and lets the DUT update itself. When the test is done the
release is deactivated and its images are deleted. The versions of the DUT are read from the
nRF Cloud shadow.

The environment of the Memfault releases is described in utils/memfault_ota.py. The test files:
 - MCUBOOT_DFU_ZIP (default artifacts/dfu_mcuboot_fw_info_v3.zip): dfu_mcuboot.zip of a build with
   a higher bootloader (fw_info) version than the baseline build.
 - artifacts/mfw_fota: the modem FOTA files, see MFW_* below.
"""

import functools
import os
import re
import sys
import time
import types

import pytest

sys.path.append(os.getcwd())

from utils.flash_tools import flash_device, reset_device
from utils.logger import get_logger
from utils.memfault_ota import OTAPayload, extract_dfu_zip

logger = get_logger()

MFW_FILEPATH = "artifacts/mfw_nrf91x1_2.0.4.zip"

# The modem FOTA files: the full image, and the deltas between the modem version of the DUT and
# a test version of it.
MFW_FOTA_DIR = "artifacts/mfw_fota"
MFW_FULL_FILE = "mfw_nrf91x1_2.0.4.cbor"
MFW_DELTA_TO_TEST_FILE = "mfw_nrf91x1_update_from_2.0.4_to_2.0.4-FOTA-TEST.bin"
MFW_DELTA_FROM_TEST_FILE = "mfw_nrf91x1_update_from_2.0.4-FOTA-TEST_to_2.0.4.bin"

# The versions the modem reports, which are the component versions of the modem in Memfault
MFW_VERSION = "mfw_nrf91x1_2.0.4"
MFW_DELTA_VERSION_FOTA_TEST = "mfw_nrf91x1_2.0.4-FOTA-TEST"
# Memfault only offers a component version that sorts after the reported one, and MFW_VERSION
# sorts before MFW_DELTA_VERSION_FOTA_TEST. The component version of an image is only a label,
# so the images get a "z" prefix to always sort last.
MFW_DELTA_VERSION_FOTA_TEST_LABEL = "z" + MFW_DELTA_VERSION_FOTA_TEST
MFW_VERSION_RESTORE_LABEL = "z" + MFW_VERSION

SOFTWARE_TYPE_APP = "app"
SOFTWARE_TYPE_MFW = "mfw"
SOFTWARE_TYPE_MCUBOOT_S0 = "mcuboot-s0"
SOFTWARE_TYPE_MCUBOOT_S1 = "mcuboot-s1"

MCUBOOT_DFU_ZIP = os.getenv("MCUBOOT_DFU_ZIP") or "artifacts/dfu_mcuboot_fw_info_v3.zip"

BOOTLOADER_VERSION_BASELINE = "2"
BOOTLOADER_VERSION_UPDATED = "3"
BOOTLOADER_FIRMWARE_VERSION_LOG = "Firmware version 3"

DEVICE_MSG_TIMEOUT = 60 * 5
APP_FOTA_TIMEOUT = 60 * 15
BOOTLOADER_FOTA_TIMEOUT = 60 * 20
DELTA_MFW_FOTA_TIMEOUT = 60 * 15
FULL_MFW_FOTA_TIMEOUT = 60 * 30
RECONNECT_TIMEOUT = 60 * 5


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


def get_bootloaderversion(dut_fota):
    shadow = dut_fota.fota.get_device(dut_fota.device_id)
    return shadow["state"]["reported"]["device"]["deviceInfo"]["bootloaderVersion"]


def _app_version_file():
    return os.path.abspath(
        os.path.join(os.path.dirname(__file__), "../../../../app/VERSION")
    )


def parse_app_version():
    """Return the application version of this checkout, as reported in the nRF Cloud shadow."""
    version = {}
    with open(_app_version_file()) as f:
        for line in f:
            match = re.match(r"^(\w+)\s*=\s*(\S+)", line.strip())
            if match:
                version[match.group(1)] = match.group(2)

    app_version = (
        f"{version['VERSION_MAJOR']}.{version['VERSION_MINOR']}.{version['PATCHLEVEL']}"
    )
    extra = version.get("EXTRAVERSION")
    if extra:
        app_version += f"-{extra}"
    return app_version


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

    flash_device(os.path.abspath(MFW_FILEPATH))
    flash_device(os.path.abspath(hex_file))

    try:
        dut_fota.uart.xfactoryreset()
        dut_fota.uart.flush()
    except Exception as e:
        logger.warning(f"Factory reset during restore failed: {e}")

    reset_device()


def trigger_fota_poll(dut_fota, max_attempts=3):
    """Poll for a FOTA update, until the DUT starts to download one."""
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
    patterns_lte_offline = ["PDN connection network detached"]
    patterns_lte_normal = ["PDN connection activated"]

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


def prepare_device(dut_fota, hex_path):
    """Flash the firmware, and have the DUT connected to the cloud from a clean state."""
    flash_device(os.path.abspath(hex_path))
    dut_fota.uart.xfactoryreset()
    dut_fota.uart.flush()
    reset_device()

    dut_fota.uart.wait_for_str_with_retries(
        "Connected to Cloud", max_retries=3, timeout=240, reset_func=reset_device)


def await_reconnect(dut_fota, start_pos, max_attempts=3):
    """Wait for the DUT to be back in the cloud after the reboot of an update."""
    for attempt in range(max_attempts):
        try:
            dut_fota.uart.wait_for_str(
                "Connected to Cloud", timeout=RECONNECT_TIMEOUT, start_pos=start_pos)
            return
        except AssertionError:
            if attempt == max_attempts - 1:
                raise
            logger.warning("Not connected to cloud after the update, resetting the device")
            reset_device()
            start_pos = dut_fota.uart.get_size()


def run_memfault_fota(dut_fota, memfault_ota, payloads, version_getter, field, expected_version,
                      timeout, resumption=None):
    """Update the DUT with a Memfault release, and verify the version it reports afterwards.

    The release has the given images. The DUT is polled for the update, downloads it, and
    reboots to install it.
    """
    with memfault_ota.release(payloads):
        trigger_fota_poll(dut_fota)

        if resumption:
            run_fota_resumption(dut_fota, resumption)

        dut_fota.uart.wait_for_str("Download complete", timeout=timeout)
        post_download_pos = dut_fota.uart.get_size()
        await_reconnect(dut_fota, post_download_pos)

        try:
            await_nrfcloud(
                functools.partial(version_getter, dut_fota),
                expected_version,
                field,
                DEVICE_MSG_TIMEOUT
            )
        except RuntimeError:
            logger.error(f"{field} is not {expected_version} after {DEVICE_MSG_TIMEOUT}s")
            raise


@pytest.fixture
def mfw_fota_files():
    """The modem FOTA files, skipping the test if they are not provided."""
    files = types.SimpleNamespace(
        full=os.path.join(MFW_FOTA_DIR, MFW_FULL_FILE),
        delta_to_test=os.path.join(MFW_FOTA_DIR, MFW_DELTA_TO_TEST_FILE),
        delta_from_test=os.path.join(MFW_FOTA_DIR, MFW_DELTA_FROM_TEST_FILE),
    )
    missing = [p for p in vars(files).values() if not os.path.isfile(p)]
    if missing:
        pytest.skip(f"Modem FOTA files not found: {', '.join(missing)}")

    return files


def update_modem_to_test_version(dut_fota, memfault_ota, mfw_fota_files):
    """Update the modem from MFW_VERSION to the test version with a delta."""
    run_memfault_fota(
        dut_fota, memfault_ota,
        [OTAPayload(mfw_fota_files.delta_to_test, SOFTWARE_TYPE_MFW,
                    component_version=MFW_DELTA_VERSION_FOTA_TEST_LABEL,
                    component_base_version=MFW_VERSION)],
        get_modemversion, "modemFirmware", MFW_DELTA_VERSION_FOTA_TEST, DELTA_MFW_FOTA_TIMEOUT)

@pytest.mark.slow
def test_app_fota_release_to_nightly(dut_fota, memfault_ota, dfu_zip_file, release_hex_file):
    '''
    Test application FOTA upgrading the most recent release to the nightly build.
    '''
    nightly_version = parse_app_version()
    release_version = release_hex_file.version

    # The version check after the update is a substring match against the
    # reported appVersion, so it only proves anything if the nightly version
    # cannot already be found in the version the device starts out with.
    assert nightly_version not in release_version, (
        f"Nightly version {nightly_version} is indistinguishable from release "
        f"version {release_version}, so the update would not be verified"
    )

    prepare_device(dut_fota, release_hex_file.path)
    app = next(e for e in extract_dfu_zip(dfu_zip_file, "artifacts/memfault_app")
               if e.get("type") == "application")

    run_memfault_fota(
        dut_fota, memfault_ota,
        [OTAPayload(app["path"], SOFTWARE_TYPE_APP)],
        get_appversion, "appVersion", nightly_version, APP_FOTA_TIMEOUT,
        resumption="app")


@pytest.mark.slow
def test_app_fota_nightly_to_nightly(
    dut_fota, memfault_ota, dfu_zip_fota_target_file, hex_file, fota_target_version
):
    '''
    Test application FOTA from the baseline nightly build to a second nightly
    build with a "fota-test" EXTRAVERSION (see build.yml), which sorts after the baseline.

    The release version is the version of the second build, as it reports it to Memfault. It is
    the release that all the FOTA tests share.
    '''
    baseline_version = parse_app_version()
    target_version = fota_target_version

    assert baseline_version != target_version, (
        f"Baseline and FOTA target versions must differ "
        f"({baseline_version!r} vs {target_version!r})"
    )
    assert target_version not in baseline_version, (
        f"Target version {target_version!r} is indistinguishable from baseline "
        f"{baseline_version!r} in the shadow appVersion substring check"
    )

    prepare_device(dut_fota, hex_file)
    app = next(e for e in extract_dfu_zip(dfu_zip_fota_target_file, "artifacts/memfault_app")
               if e.get("type") == "application")

    logger.info(f"Upgrading nightly {baseline_version} to {target_version}")
    run_memfault_fota(
        dut_fota, memfault_ota,
        [OTAPayload(app["path"], SOFTWARE_TYPE_APP)],
        get_appversion, "appVersion", target_version, APP_FOTA_TIMEOUT,
        resumption="app")


@pytest.mark.slow
def test_bootloader_fota(dut_fota, memfault_ota, hex_file):
    '''
    Test MCUboot bootloader (B1) FOTA.

    The images of both bootloader slots are uploaded, as software types of their own, with the
    bootloader version as the component version. The DUT updates the inactive slot.
    '''
    if not os.path.isfile(MCUBOOT_DFU_ZIP):
        pytest.skip(f"{MCUBOOT_DFU_ZIP} not found")

    images = extract_dfu_zip(MCUBOOT_DFU_ZIP, "artifacts/memfault_mcuboot")
    software_types = {"0": SOFTWARE_TYPE_MCUBOOT_S0, "1": SOFTWARE_TYPE_MCUBOOT_S1}
    versions = {e["version_B0"] for e in images}
    assert versions == {BOOTLOADER_VERSION_UPDATED}, (
        f"{MCUBOOT_DFU_ZIP} has bootloader version(s) {versions}, "
        f"expected {BOOTLOADER_VERSION_UPDATED}")
    payloads = [
        OTAPayload(e["path"], software_types[e["slot"]], component_version=e["version_B0"])
        for e in images
    ]

    try:
        prepare_device(dut_fota, hex_file)

        await_bootloader_version(dut_fota, BOOTLOADER_VERSION_BASELINE)

        with memfault_ota.release(payloads):
            trigger_fota_poll(dut_fota)

            dut_fota.uart.wait_for_str("Download complete", timeout=BOOTLOADER_FOTA_TIMEOUT)
            post_download_pos = dut_fota.uart.get_size()

            dut_fota.uart.wait_for_str(
                BOOTLOADER_FIRMWARE_VERSION_LOG,
                timeout=BOOTLOADER_FOTA_TIMEOUT,
                start_pos=post_download_pos,
                error_msg="Expected B0 fw_info v3 after download",
            )

            await_reconnect(dut_fota, post_download_pos)

            await_bootloader_version(dut_fota, BOOTLOADER_VERSION_UPDATED,
                                     timeout=BOOTLOADER_FOTA_TIMEOUT)
    finally:
        flash_device(os.path.abspath(hex_file))


def test_delta_mfw_fota(dut_fota, memfault_ota, mfw_fota_files, hex_file):
    '''
    Test delta modem FOTA on nrf9151: to the test version of the modem, and back.
    '''
    try:
        prepare_device(dut_fota, hex_file)

        update_modem_to_test_version(dut_fota, memfault_ota, mfw_fota_files)

        logger.info("Running a second delta fota back from FOTA-TEST")
        run_memfault_fota(
            dut_fota, memfault_ota,
            [OTAPayload(mfw_fota_files.delta_from_test, SOFTWARE_TYPE_MFW,
                        component_version=MFW_VERSION_RESTORE_LABEL,
                        component_base_version=MFW_DELTA_VERSION_FOTA_TEST)],
            get_modemversion, "modemFirmware", MFW_VERSION, DELTA_MFW_FOTA_TIMEOUT)
    finally:
        restore_device_after_modem_fota(dut_fota, hex_file)


@pytest.mark.slow
def test_full_mfw_fota(dut_fota, memfault_ota, mfw_fota_files, hex_file):
    '''
    Test full modem FOTA on nrf9151, after a delta modem FOTA with the same software type.

    A full and a delta modem image are served as the same software type, and the device finds
    out the kind of image when it has downloaded it (NRF_CLOUD_FOTA_MODEM_ANY). The modem is
    first updated with a delta to the test version, as a full update to the version it already
    has would not be offered.
    '''
    try:
        prepare_device(dut_fota, hex_file)

        update_modem_to_test_version(dut_fota, memfault_ota, mfw_fota_files)

        logger.info("Running a full modem fota from FOTA-TEST")
        dut_fota.uart.flush()
        run_memfault_fota(
            dut_fota, memfault_ota,
            [OTAPayload(mfw_fota_files.full, SOFTWARE_TYPE_MFW,
                        component_version=MFW_VERSION_RESTORE_LABEL)],
            get_modemversion, "modemFirmware", MFW_VERSION, FULL_MFW_FOTA_TIMEOUT,
            resumption="full")
    finally:
        restore_device_after_modem_fota(dut_fota, hex_file)
