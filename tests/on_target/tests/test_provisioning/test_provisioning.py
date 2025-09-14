import os
import sys
import json
import time
import pytest
import requests.exceptions  # Used for handling HTTP errors from nRF Cloud API

# Ensure the utils directory is in the Python path
sys.path.append(os.getcwd())

from utils.flash_tools import flash_device, reset_device
from utils.logger import get_logger

logger = get_logger()

# Default nRF Cloud CoAP security tag used for device credentials
SEC_TAG = 16842753

# --- Helper Functions ---


def _perform_initial_device_setup_and_factory_reset(dut_cloud, hex_file: str):
    logger.info(f"Flashing device with {hex_file} and performing factory reset.")

    flash_device(os.path.abspath(hex_file))
    dut_cloud.uart.xfactoryreset()
    dut_cloud.uart.flush()
    reset_device()


def _wait_for_lte_connection(dut_cloud, timeout: int = 240):
    logger.info("Waiting for device to connect to LTE network...")

    log_pattern_network_connected = "network: Network connectivity established"
    dut_cloud.uart.wait_for_str(
        log_pattern_network_connected,
        timeout=timeout,
        start_pos=dut_cloud.uart.get_size(),
    )

    logger.info("Device connected to LTE network.")


def _disconnect_network_and_clear_modem_credentials(dut_cloud, sec_tag: int):
    logger.info("Disconnecting network and clearing modem credentials...")

    log_pattern_network_disconnected = "network: Network connectivity lost"
    dut_cloud.uart.write("att_network disconnect\r\n")
    dut_cloud.uart.wait_for_str(
        log_pattern_network_disconnected,
        timeout=20,
        start_pos=dut_cloud.uart.get_size(),
    )
    dut_cloud.uart.write(f"at AT%CMNG=1\r\n")
    time.sleep(1)
    # Clear any existing credentials for the given security tag
    dut_cloud.uart.write(f"at AT%CMNG=3,{sec_tag},0\r\n")
    time.sleep(1)
    dut_cloud.uart.write(f"at AT%CMNG=3,{sec_tag},1\r\n")
    time.sleep(1)
    dut_cloud.uart.write(f"at AT%CMNG=3,{sec_tag},2\r\n")
    time.sleep(1)

    logger.info("Modem credentials cleared.")


def _get_attestation_token_from_device(dut_cloud) -> str:
    logger.info("Getting attestation token from device...")

    dut_cloud.uart.at_cmd_write("at AT%ATTESTTOKEN\r\n")
    token_match = dut_cloud.uart.wait_for_str_re(
        r'%ATTESTTOKEN: "([^"]+)"', timeout=20
    )

    assert token_match, "No attestation token found"
    attestation_token = token_match[0]
    logger.info(f"Attestation Token: {attestation_token}")

    return attestation_token


def _unclaim_device_from_nrf_cloud_if_exists(dut_cloud):
    """
    Attempts to unclaim the device from nRF Cloud.
    If the device is not found (404), it's considered a success (already unclaimed).
    """
    logger.info(f"Attempting to unclaim device {dut_cloud.device_id} from nRF Cloud...")

    try:
        status_code = dut_cloud.cloud.unclaim_device(device_id=dut_cloud.device_id)
        logger.info(f"Unclaim device status_code: {status_code}")
    except requests.exceptions.HTTPError as e:
        if e.response.status_code == 404:
            logger.info(
                f"Device {dut_cloud.device_id} not found or already unclaimed (404), proceeding."
            )
        else:
            logger.error(f"Error unclaiming device: {e}")
            raise  # Re-raise other HTTP errors


def _connect_to_network_and_wait_for_claiming_prompt(dut_cloud):
    logger.info("Connecting to network and waiting for device to request claiming...")

    log_pattern_network_connected = "network: Network connectivity established"
    log_pattern_need_claiming = (
        "Claim the device using the device's attestation token on nrfcloud.com"
    )

    dut_cloud.uart.write("att_network connect\r\n")
    dut_cloud.uart.wait_for_str(
        log_pattern_network_connected, timeout=240, start_pos=dut_cloud.uart.get_size()
    )
    dut_cloud.uart.wait_for_str(
        log_pattern_need_claiming, timeout=240, start_pos=dut_cloud.uart.get_size()
    )

    logger.info("Device is ready to be claimed.")


def _claim_device_on_nrf_cloud(dut_cloud, attestation_token: str):
    logger.info("Claiming device on nRF Cloud...")

    dut_cloud.cloud.claim_device(attestation_token=attestation_token)

    logger.info("Device claimed successfully.")


def _wait_for_provisioning_completion_and_cloud_connection(
    dut_cloud, timeout: int = 240
):
    logger.info(
        "Waiting for provisioning to complete and device to connect to nRF Cloud..."
    )

    dut_cloud.uart.wait_for_str(
        [
            "cloud: nrf_provisioning_callback: Provisioning finished",
            "cloud: Connected to Cloud",
        ],
        timeout=timeout,
        start_pos=dut_cloud.uart.get_size(),
    )

    logger.info("Device provisioned and connected to nRF Cloud.")


def _trigger_device_reprovisioning_with_new_credentials(dut_cloud, sec_tag: int):
    """
    Initiates the reprovisioning process on the device by:
    1. Adding a reprovisioning command (new credentials) to nRF Cloud.
    2. Updating the device shadow to signal the new command.
    3. Simulating a button press on the device to trigger command processing.
    """
    logger.info("Starting reprovisioning process with new credentials...")

    # Prepare the reprovisioning command for nRF Cloud
    command_json = json.dumps(
        {
            "description": "Reprovisioning with new cloud credentials",
            "request": {"cloudAccessKeyGeneration": {"secTag": sec_tag}},
        }
    )

    logger.info(f"Adding reprovisioning command to nRF Cloud: {command_json}")

    dut_cloud.cloud.add_provisioning_command(
        device_id=dut_cloud.device_id, command=command_json
    )

    # Update device shadow to indicate a new provisioning command is available
    logger.info("Updating device shadow to trigger reprovisioning command processing.")

    dut_cloud.cloud.patch_delete_command_entry_from_shadow(
        device_id=dut_cloud.device_id
    )
    dut_cloud.cloud.patch_add_provisioning_command_to_shadow(
        device_id=dut_cloud.device_id,
        command=1,  # Command ID 1 signifies a provisioning request
    )

    # Simulate a button press to make the device check for new commands
    logger.info("Simulating button press to trigger command processing on device.")

    dut_cloud.uart.write("att_button_press 1\r\n")


def _trigger_device_reprovisioning_expecting_no_commands(dut_cloud):
    """
    Initiates a reprovisioning check on the device when no new commands are expected from nRF Cloud.
    This tests the device's behavior when it checks for commands but finds none.
    """
    logger.info("Starting reprovisioning process expecting no new commands...")

    # Update device shadow to indicate a provisioning check (even if no commands are queued)
    logger.info("Updating device shadow to trigger provisioning check.")

    dut_cloud.cloud.patch_delete_command_entry_from_shadow(
        device_id=dut_cloud.device_id
    )
    dut_cloud.cloud.patch_add_provisioning_command_to_shadow(
        device_id=dut_cloud.device_id,
        command=1,  # Command ID 1 typically signifies a provisioning request
    )

    # Simulate a button press to make the device check for commands
    logger.info("Simulating button press to trigger command processing on device.")

    dut_cloud.uart.write("att_button_press 1\r\n")


# --- Test Phases ---


def _run_initial_provisioning(dut_cloud, hex_file):
    logger.info("--- Starting Phase 1: Initial Device Provisioning ---")

    _perform_initial_device_setup_and_factory_reset(dut_cloud, hex_file)
    _wait_for_lte_connection(dut_cloud)
    _disconnect_network_and_clear_modem_credentials(dut_cloud, SEC_TAG)
    attestation_token = _get_attestation_token_from_device(dut_cloud)
    _unclaim_device_from_nrf_cloud_if_exists(dut_cloud)
    _connect_to_network_and_wait_for_claiming_prompt(dut_cloud)
    _claim_device_on_nrf_cloud(dut_cloud, attestation_token)
    _wait_for_provisioning_completion_and_cloud_connection(dut_cloud)

    logger.info("--- Phase 1: Initial Device Provisioning Completed Successfully ---")


def _run_reprovisioning(dut_cloud):
    logger.info("--- Starting Phase 2: Reprovisioning with New Credentials ---")

    dut_cloud.uart.flush()

    _trigger_device_reprovisioning_with_new_credentials(dut_cloud, SEC_TAG)
    # Wait for the device to process the command, reprovision, and reconnect
    _wait_for_provisioning_completion_and_cloud_connection(dut_cloud, timeout=300)

    logger.info(
        "--- Phase 2: Reprovisioning with New Credentials Completed Successfully ---"
    )


def _run_reprovisioning_expecting_no_commands(dut_cloud):
    logger.info("--- Starting Phase 3: Reprovisioning Expecting No Commands ---")

    dut_cloud.uart.flush()

    _trigger_device_reprovisioning_expecting_no_commands(dut_cloud)
    # Verify the device correctly handles the absence of new provisioning commands
    dut_cloud.uart.wait_for_str(
        [
            "cloud: No commands from the nRF Provisioning Service to process",
            "cloud: Connected to Cloud",
        ],
        timeout=300,
        start_pos=dut_cloud.uart.get_size(),
    )
    logger.info(
        "--- Phase 3: Reprovisioning Expecting No Commands Completed Successfully ---"
    )


# --- Main Test ---
@pytest.mark.parametrize("_", range(1, 3))
def test_device_provisioning(_, dut_cloud, hex_file):
    """
    Tests the full device provisioning and reprovisioning lifecycle:
    1. Initial provisioning: Flashes, gets attestation token, claims on nRF Cloud, connects.
    2. Reprovisioning with new credentials: Device receives request for reprovisioning via the device shadow,
       connects to nRF Cloud Provsioning Service, and handles the new provisioning commands.
    3. Reprovisioning without commands: Device receives request for reprovisioning via the device shadow,
       connects to nRF Cloud Provisioning Service, but finds no new commands to process and establishes
       a connection without reprovisioning.
    """
    _run_initial_provisioning(dut_cloud, hex_file)
    _run_reprovisioning(dut_cloud)
    _run_reprovisioning_expecting_no_commands(dut_cloud)
