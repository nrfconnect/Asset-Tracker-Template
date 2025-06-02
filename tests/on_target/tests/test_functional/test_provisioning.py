import pytest
import time
from nrfcloud.api import NrfCloudProvisioningAPI, NrfCloudDeviceAPI
from nrfcloud.at_client import ATClient
from nrfcloud.utils import wait_for_device_state, wait_for_device_data

SEC_TAG = 6667772

def test_device_provisioning(dut_cloud, hex_file):
        flash_device(os.path.abspath(hex_file))
        dut_board.uart.xfactoryreset()
        dut_board.uart.flush()
        reset_device()

        # Setup test

        # Define log patterns
        log_pattern_network_connected = "network: Network connectivity established"
        log_pattern_network_disconnected = "network: Network connectivity lost"

        # 1. Wait for device to connect to LTE network
        dut_board.uart.wait_for_str(log_pattern_network_connected, timeout=240)

        # 2. Disconnect from network and wait for disconnection
        dut_fota.uart.write("att_network disconnect\r\n")
        dut_fota.uart.wait_for_str(log_pattern_network_disconnected, timeout=20)

        # 3. Delete security tag using AT commands, don't care about the return value as credentials
        #    in that sec tag might (not) be present
        dut_board.at_cmd_write("AT+CMNG=3,{},0".format(SEC_TAG))
        dut_board.at_cmd_write("AT+CMNG=3,{},1".format(SEC_TAG))
        dut_board.at_cmd_write("AT+CMNG=3,{},2".format(SEC_TAG))

        # 4. Get the device device UUID
        dut_board.at_cmd_write("AT%XMODEMUUID")

        values = dut_board.uart.extract_value(r'%XMODEMUUID: ([^"]+)')
        assert values

        device_uuid = values[0]
        assert device_uuid

        # 5. Get the attestation token
        dut_board.at_cmd_write("AT%ATTESTTOKEN")

        values = dut_board.uart.extract_value(r'%ATTESTTOKEN: "([^"]+)"')
        assert values

        attestation_token = values[0]
        assert attestation_token

        # 6. Delete the device from the nRF Provisioning Service
        dut_cloud.cloud.delete_provisioned_device(
            device_uuid=device_uuid
        )

        # 7. Provision the device using the nRF Provisioning Service
        dut_cloud.cloud.claim_device(
            device_uuid=device_uuid,
            attestation_token=attestation_token
        )

        # 8. Connect the device to the network and wait for it to provision
        dut_board.uart.write("att_network connect\r\n")
        dut_board.uart.wait_for_str(log_pattern_network_connected, timeout=240)

        # 9. Wait for the device to be provisioned
        dut_board.uart_wait_for_str("cloud: Provisioning successful", timeout=240)

        # 10. Wait for the device to be connected to the nRF Cloud
        dut_board.uart_wait_for_str("cloud: Connected to Cloud", timeout=240)

###### Reprovisioning ######
