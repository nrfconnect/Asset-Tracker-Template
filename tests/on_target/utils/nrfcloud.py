##########################################################################################
# Copyright (c) 2025 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import json
import time
import random
import subprocess
import requests
from typing import Callable
from datetime import datetime, timedelta, timezone
from utils.logger import get_logger
from requests.exceptions import ConnectionError, Timeout

logger = get_logger()

DEFAULT_MAX_RETRIES = 3
DEFAULT_RETRY_DELAY_SECONDS = 2
RETRYABLE_STATUS_CODES = {500, 502, 503, 504}

class NRFCloudFOTAError(Exception):
    pass

class NRFCloud():
    def __init__(
        self, api_key: str="", url: str="https://api.nrfcloud.com/v1", timeout: int=10,
        nrfcloud_org_token: str=None, nrfcloud_org: str=None, nrfcloud_project: str=None,
    ) -> None:
        """ Initalizes the class """
        self.url = url
        # Time format used by nrfcloud.com (API may omit fractional seconds)
        self.time_fmt = '%Y-%m-%dT%H:%M:%S.%fZ'
        self.time_fmt_no_frac = '%Y-%m-%dT%H:%M:%SZ'
        self.default_headers = {
            'Authorization': "Bearer " + api_key,
            'Accept':'application/json',
            "Content-Type": "application/json"
        }
        self.session = requests.Session()
        self.session.headers.update(self.default_headers)
        self.timeout = timeout

        self.nrfcloud_org_token = nrfcloud_org_token
        self.nrfcloud_org = nrfcloud_org
        self.nrfcloud_project = nrfcloud_project

    def _ota_cli_base(self) -> list:
        return [
            "memfault",
            "--org-token", self.nrfcloud_org_token,
            "--org", self.nrfcloud_org,
            "--project", self.nrfcloud_project,
        ]

    def _run_ota_cli(self, args: list, check: bool = True) -> subprocess.CompletedProcess:
        """Run the OTA CLI, logging its output so CI shows what happened."""
        result = subprocess.run([*self._ota_cli_base(), *args], capture_output=True, text=True)
        output = "\n".join(part.strip() for part in (result.stdout, result.stderr) if part.strip())
        if output:
            logger.info(f"OTA CLI output:\n{output}")
        if check and result.returncode != 0:
            raise NRFCloudFOTAError(
                f"OTA CLI command {args} failed (rc={result.returncode}): {output}"
            )
        return result

    def upload_ota_payload(
        self, bin_path: str, hardware_version: str, software_type: str, software_version: str,
    ) -> None:
        """Upload a firmware binary as an OTA payload."""
        self._run_ota_cli([
            "upload-ota-payload",
            "--hardware-version", hardware_version,
            "--software-type", software_type,
            "--software-version", software_version,
            bin_path,
        ])

    def deploy_release(self, cohort: str, software_version: str) -> None:
        """Deploy a release to a cohort, tolerating one already active."""
        result = self._run_ota_cli([
            "deploy-release",
            "--release-version", software_version,
            "--cohort", cohort,
        ], check=False)
        if result.returncode == 0:
            return
        if "already active" in f"{result.stdout}\n{result.stderr}":
            return
        raise NRFCloudFOTAError(
            f"Failed to deploy release {software_version} to cohort {cohort}: "
            f"{result.stdout}\n{result.stderr}"
        )

    def deactivate_release(self, cohort: str, software_version: str) -> None:
        """Deactivate a release from a cohort, if active."""
        self._run_ota_cli([
            "deploy-release",
            "--release-version", software_version,
            "--cohort", cohort,
            "--deactivate",
        ])

    def _request_with_retry(self, method: Callable, path: str, return_json: bool = False, **kwargs):
        """
        Execute an HTTP request with retry logic.

        Retries on 5xx server errors and connection/timeout issues with exponential backoff.
        """
        for attempt in range(DEFAULT_MAX_RETRIES):
            try:
                r = method(url=self.url + path, **kwargs, timeout=self.timeout)
                if r.status_code in RETRYABLE_STATUS_CODES:
                    logger.warning(f"Retryable status {r.status_code} on attempt {attempt + 1}/{DEFAULT_MAX_RETRIES} for {path}")
                    if attempt < DEFAULT_MAX_RETRIES - 1:
                        delay = DEFAULT_RETRY_DELAY_SECONDS * (2 ** attempt) + random.uniform(0, 1)
                        time.sleep(delay)
                        continue
                r.raise_for_status()
                return r.json() if return_json else r
            except (ConnectionError, Timeout) as e:
                logger.warning(f"Connection error on attempt {attempt + 1}/{DEFAULT_MAX_RETRIES} for {path}: {e}")
                if attempt < DEFAULT_MAX_RETRIES - 1:
                    delay = DEFAULT_RETRY_DELAY_SECONDS * (2 ** attempt) + random.uniform(0, 1)
                    time.sleep(delay)
                    continue
                raise
        # If we exit the loop due to retryable status codes, raise the last response's status
        r.raise_for_status()

    def _parse_timestamp(self, timestamp_str: str) -> datetime:
        for fmt in (self.time_fmt, self.time_fmt_no_frac):
            try:
                return datetime.strptime(timestamp_str, fmt).replace(tzinfo=timezone.utc)
            except ValueError:
                continue
        raise ValueError(f"Unsupported timestamp format: {timestamp_str}")

    def _get(self, path: str, **kwargs) -> dict:
        return self._request_with_retry(self.session.get, path, return_json=True, **kwargs)

    def _post(self, path: str, **kwargs):
        return self._request_with_retry(self.session.post, path, **kwargs)

    def _put(self, path: str, **kwargs):
        return self._request_with_retry(self.session.put, path, **kwargs)

    def _delete(self, path: str, **kwargs):
        return self._request_with_retry(self.session.delete, path, **kwargs)

    def _patch(self, path: str, **kwargs):
        return self._request_with_retry(self.session.patch, path, **kwargs)

    def claim_device(self, attestation_token: str) -> None:
        """
        Add (claim) a provisioned device to nrfcloud.com

        :param attestation_token: Attestation token for device
        :return: None
        """
        data = json.dumps({
            "claimToken": attestation_token,
            "tags": ["nrf-cloud-onboarding"]
        })

        # Use the provisioning API endpoint for unclaiming
        original_url = self.url
        self.url = "https://api.provisioning.nrfcloud.com/v1"
        try:
            self._post(path=f"/claimed-devices", data=data)
        finally:
            self.url = original_url

    def unclaim_device(self, device_id: str) -> int:
        """
        Unclaim (delete) a claimed device from nrfcloud.com

        :param device_id: Device ID
        :return: HTTP status code from the delete call
        """
        # Use the provisioning API endpoint for unclaiming
        original_url = self.url
        self.url = "https://api.provisioning.nrfcloud.com/v1"
        try:
            response = self._delete(path=f"/claimed-devices/{device_id}")
            return response.status_code
        finally:
            self.url = original_url

    def add_provisioning_command(self, device_id: str, command: str) -> None:
        """
        Add a provisioning command to a claimed device.

        :param device_id: Device ID
        :param command: Command as a JSON string
        :return: None
        """

        data = command  # command is already a JSON string containing all needed data

        # Use the provisioning API endpoint for unclaiming
        original_url = self.url
        self.url = "https://api.provisioning.nrfcloud.com/v1"
        try:
            self._post(path=f"/claimed-devices/{device_id}/provisioning", data=data)
        finally:
            self.url = original_url

    def get_devices(self, path: str="", params=None) -> dict:
        return self._get(path=f"/devices{path}", params=params)

    def get_device(self, device_id: str, params=None) -> dict:
        """
        Get all information about particular device on nrfcloud.com

        :param device_id: Device ID
        :return: Json structure of result from nrfcloud.com
        """
        return self.get_devices(path=f"/{device_id}", params=params)

    def get_messages(self, device: str=None, appname: str="donald", max_records: int=50, max_age_hrs: int=24) -> list:
        """
        Get messages sent from asset_tracker to nrfcloud.com

        :param device_id: Limit result to messages from particular device
        :param max_records: Limit number of messages to fetch
        :param max_age_hrs: Limit fetching messages by timestamp
        :return: List of (timestamp, message)
        """
        end = datetime.now(timezone.utc).strftime(self.time_fmt)
        start = (datetime.now(timezone.utc) - timedelta(
            hours=max_age_hrs)).strftime(self.time_fmt)
        params = {
            'start': start,
            'end': end,
            'pageSort': 'desc',
            'pageLimit': max_records
        }

        if device:
            params['deviceId'] = device
        if appname:
            params['appId'] = appname

        messages = self._get(path="/messages", params=params)

        return [(self._parse_timestamp(x['receivedAt']), x['message'])
            for x in messages['items']]

    def check_message_age(self, message: dict, hours: int=0, minutes: int=0, seconds: int=0) -> bool:
        """
        Check age of message, return False if message older than parameters

        :param messages: Single message
        :param hours: Max message age hours
        :param minutes: Max message age minutes
        :param seconds: Max message age seconds
        :return: bool True/False
        """
        diff = timedelta(hours=hours, minutes=minutes, seconds=seconds)
        return datetime.now(timezone.utc) - message[0] < diff

    def patch_config(self, device_id: str, sample_interval: int, storage_threshold: int) -> None:
        """
        Update the device's configuration (sample_interval, storage_threshold)

        :param device_id: Device ID to update
        :param sample_interval: New sample interval in seconds
        :param storage_threshold: New storage threshold in samples
        """
        data = json.dumps({
            "desired": {
                "config": {
                    "sample_interval": sample_interval,
                    "storage_threshold": storage_threshold
                }
            }
        })
        return self._patch(f"/devices/{device_id}/state", data=data)

    def patch_add_provisioning_command_to_shadow(self, device_id: str, command: int) -> None:
        """
        Update the device's update interval configuration

        :param device_id: Device ID to update
        :param interval: New update interval in seconds
        """
        data = json.dumps({
            "desired": {
                "command": [command, random.randint(1, 100)]
            }
        })
        return self._patch(f"/devices/{device_id}/state", data=data)

    def patch_delete_command_entry_from_shadow(self, device_id: str) -> None:
        """
        Delete a specific desired state key for a device

        :param device_id: Device ID to update
        :param key: Desired state key to delete
        """
        data = json.dumps({
            "desired": {
                "command": None,
            },
            "reported": {
                "command": None,
            }
        })
        return self._patch(f"/devices/{device_id}/state", data=data)

    def patch_reset_config_and_command(self, device_id: str) -> None:
        """
        Null out the `config` and `command` sections from both `desired` and `reported`
        shadow state so that the device starts from a clean slate. This is useful for
        tests that need deterministic shadow content on first connect.

        :param device_id: Device ID to reset
        """
        data = json.dumps({
            "desired": {
                "config": None,
                "command": None,
            },
            "reported": {
                "config": None,
                "command": None,
            }
        })
        return self._patch(f"/devices/{device_id}/state", data=data)

