import pytest
import time
import os
import functools
from utils.flash_tools import flash_device, reset_device
from utils.nrfcloud import NRFCloudFOTAError
import sys
sys.path.append(os.getcwd())
from utils.logger import get_logger
import re
import requests.exceptions # Added import

# Default nRF Cloud CoAP security tag
SEC_TAG = 16842753

@pytest.mark.provisioning
def test_device_provisioning(dut_cloud, hex_file):
        flash_device(os.path.abspath(hex_file))
        dut_cloud.uart.xfactoryreset()
        dut_cloud.uart.flush()
        reset_device()

        # Define log patterns
        log_pattern_network_module_started = "network: state_disconnected_entry: state_disconnected_entry"
        log_pattern_network_connected = "network: Network connectivity established"
        log_pattern_network_disconnected = "network: Network connectivity lost"
        log_pattern_need_claiming = "Claim the device using the device's attestation token on nrfcloud.com"

        # 1. Wait for device to connect to LTE network
        dut_cloud.uart.wait_for_str(log_pattern_network_connected, timeout=240)

        # 2. Stop the network module from searching for networks, putting modem into offline mode
        dut_cloud.uart.write("att_network disconnect\r\n")
        dut_cloud.uart.wait_for_str(log_pattern_network_disconnected, timeout=20)

        dut_cloud.uart.write("at AT%CMNG=3,{},0\r\n".format(SEC_TAG))
        dut_cloud.uart.write("at AT%CMNG=3,{},1\r\n".format(SEC_TAG))
        dut_cloud.uart.write("at AT%CMNG=3,{},2\r\n".format(SEC_TAG))

        # 4. Get the attestation token

        dut_cloud.uart.at_cmd_write("at AT%ATTESTTOKEN\r\n")
        token = dut_cloud.uart.extract_value_all(r'%ATTESTTOKEN: "([^"]+)"')

        assert token, "No attestation token found"
        print(f"Attestation Token: {token[0]}")

        # 5. Delete device from nRF Cloud provisioning service
        try:
            status_code = dut_cloud.cloud.unclaim_device(
                device_id=dut_cloud.device_id,
            )
            print(f"Unclaim device status_code: {status_code}")
        except requests.exceptions.HTTPError as e:
            if e.response.status_code == 404:
                print(f"Device {dut_cloud.device_id} not found or already unclaimed (404), proceeding.")
            else:
                # Re-raise other HTTP errors
                raise

        # 7. Connect the device to the network, it shall enter provisioning mode and wait until the device is claimed
        dut_cloud.uart.write("att_network connect\r\n")
        dut_cloud.uart.wait_for_str(log_pattern_network_connected, timeout=240)

        dut_cloud.uart.wait_for_str(log_pattern_need_claiming, timeout=240)

        # 8. Provision the device using the nRF Provisioning Service
        dut_cloud.cloud.claim_device(
            attestation_token=token[0],
        )

        # 9. Wait for the device to be provisioned
        dut_cloud.uart.wait_for_str("cloud: Provisioning finished", timeout=240)

        # 10. Wait for the device to be connected to the nRF Cloud
        dut_cloud.uart.wait_for_str("cloud: Connected to Cloud", timeout=240)

        pattern_location = "location_event_handler: Got location: lat:"

        # Sampling
        dut_cloud.uart.wait_for_str(pattern_location, timeout=300)

        # Extract coordinates from UART output
        values = dut_cloud.uart.extract_value( \
            r"location_event_handler: Got location: lat: ([\d.]+), lon: ([\d.]+), acc: ([\d.]+), method:")
        assert values

        lat, lon, acc = values
        assert abs(float(lat) - 61.5) < 2 and abs(float(lon) - 10.5) < 1

        # REPROVISIONING
        # 11. Add provisioning commands to the device Cloud Access Key generation and Server Certificate

        dut_cloud.cloud.add_provisioning_command(
            device_id=dut_cloud.device_id,
            command = {"description":"","request":{"cloudAccessKeyGeneration":{"secTag": SEC_TAG}}}
        )

        dut_cloud.cloud.add_provisioning_command(
            device_id=dut_cloud.device_id,
            command = {"description":"","request":{"serverCertificate":{"secTag": SEC_TAG,"content":"-----BEGIN CERTIFICATE-----\\nMIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF\\nADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6\\nb24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL\\nMAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv\\nb3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj\\nca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM\\n9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw\\nIFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6\\nVOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L\\n93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm\\njgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC\\nAYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA\\nA4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI\\nU5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs\\nN+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv\\no/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU\\n5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy\\nrqXRfboQnoZsG4q5WTP468SQvvG5\\n-----END CERTIFICATE-----\\n\\n-----BEGIN CERTIFICATE-----\\nMIIBjzCCATagAwIBAgIUOEakGUS/7BfSlprkly7UK43ZAwowCgYIKoZIzj0EAwIw\\nFDESMBAGA1UEAwwJblJGIENsb3VkMB4XDTIzMDUyNDEyMzUzMloXDTQ4MTIzMDEy\\nMzUzMlowFDESMBAGA1UEAwwJblJGIENsb3VkMFkwEwYHKoZIzj0CAQYIKoZIzj0D\\nAQcDQgAEPVmJXT4TA1ljMcbPH0hxlzMDiPX73FHsdGM/6mqAwq9m2Nunr5/gTQQF\\nMBUZJaQ/rUycLmrT8i+NZ0f/OzoFsKNmMGQwHQYDVR0OBBYEFGusC7QaV825v0Ci\\nqEv2m1HhiScSMB8GA1UdIwQYMBaAFGusC7QaV825v0CiqEv2m1HhiScSMBIGA1Ud\\nEwEB/wQIMAYBAf8CAQAwDgYDVR0PAQH/BAQDAgGGMAoGCCqGSM49BAMCA0cAMEQC\\nIH/C3yf5aNFSFlm44CoP5P8L9aW/5woNrzN/kU5I+H38AiAwiHYlPclp25LgY8e2\\nn7e2W/H1LXJ7S3ENDBwKUF4qyw==\\n-----END CERTIFICATE-----\\n"}}}
        )

        # 12. Issue a command to the device to perform provisioning
        dut_cloud.cloud.patch_delete_desired()
        dut_cloud.cloud.patch_update_command()

        # Make the test fail here

        # assert False, "This test is not fully implemented yet. Please implement the missing parts."

        # assert false, "This test is not fully implemented yet. Please implement the missing parts."

###### Reprovisioning ######
