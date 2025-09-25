##########################################################################################
# Copyright (c) 2025 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import zipfile
import io
import re
import json
import time
import random
import requests
from enum import Enum
from typing import Union
from datetime import datetime, timedelta, timezone
from utils.logger import get_logger
from requests.exceptions import HTTPError

logger = get_logger()

class FWType(Enum):
    app = 'application'
    bootloader = 'mcuboot'
    mfw = 'modem'

class NRFCloudFOTAError(Exception):
    pass

class NRFCloud():
    def __init__(self, api_key: str, url: str="https://api.nrfcloud.com/v1", timeout: int=10) -> None:
        """ Initalizes the class """
        self.url = url
        # Time format used by nrfcloud.com
        self.time_fmt = '%Y-%m-%dT%H:%M:%S.%fZ'
        self.default_headers = {
            'Authorization': "Bearer " + api_key,
            'Accept':'application/json',
            "Content-Type": "application/json"
        }
        self.session = requests.Session()
        self.session.headers.update(self.default_headers)
        self.timeout = timeout

    def _get(self, path: str, **kwargs) -> dict:
        r = self.session.get(url=self.url + path, **kwargs, timeout=self.timeout)
        r.raise_for_status()
        return r.json()

    def _post(self, path: str, **kwargs):
        r = self.session.post(self.url + path, **kwargs, timeout=self.timeout)
        r.raise_for_status()
        return r

    def _put(self, path: str, **kwargs):
        r = self.session.put(self.url + path, **kwargs, timeout=self.timeout)
        r.raise_for_status()
        return r

    def _delete(self, path: str, **kwargs):
        r = self.session.delete(self.url + path, **kwargs, timeout=self.timeout)
        r.raise_for_status()
        return r

    def _patch(self, path: str, **kwargs):
        r = self.session.patch(self.url + path, **kwargs, timeout=self.timeout)
        r.raise_for_status()
        return r

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

        timestamp = lambda x: datetime.strptime(x['receivedAt'], self.time_fmt)
        messages = self._get(path="/messages", params=params)

        return [(timestamp(x), x['message'])
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
        return datetime.now(timezone.utc) - message[0].replace(tzinfo=timezone.utc) < diff

    def patch_update_interval(self, device_id: str, interval: int) -> None:
        """
        Update the device's update interval configuration

        :param device_id: Device ID to update
        :param interval: New update interval in seconds
        """
        data = json.dumps({
            "desired": {
                "config": {
                    "update_interval": interval
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

class NRFCloudFOTA(NRFCloud):
    def upload_firmware(
        self, name: str, bin_file: str, version: str, description: str, fw_type: FWType, bin_file_2=None
    ) -> str:
        """
        Upload firmware for FOTA

        :param name: Name as shown in nrfCloud UI
        :param bin_file: Path to binary firmware image
        :param bin_file_2: Path to the second binary firmware image
        :param version: Firmware version as shown in nrfCloud UI
        :param description: Description as shown in nrfCloud UI
        :param fw_type: Update type, bootloader|modem|application
        :return: NRFCloud bundleId parameter"
        """
        with open(bin_file, "rb") as f:
            data = f.read()
        manifest = {
            "name": name,
            "description": description,
            "fwversion": version,
            "format-version": 1,
            "files": [
                {
                    "file": bin_file.split("/")[-1],
                    "type": fw_type.value,
                    "size": len(data),
                }
            ],
        }
        fd = io.BytesIO()
        z = zipfile.ZipFile(fd, "w")
        z.writestr(bin_file.split("/")[-1], data)
        if bin_file_2:
            with open(bin_file_2, "rb") as f2:
                data2 = f2.read()
            file2 = {
                "file": bin_file_2.split("/")[-1],
                "type": fw_type.value,
                "size": len(data2),
            }
            manifest["files"].append(file2)
            z.writestr(bin_file_2.split("/")[-1], data2)
        z.writestr("manifest.json", json.dumps(manifest))
        z.close()
        fd.seek(0)
        headers = {
            "Content-Type": "application/zip"
        }
        r = self._post("/firmwares", headers=headers, data=fd.read())
        uris = r.json()["uris"]
        if fw_type == FWType.app:
            m = re.match(r"https://(firmware|bundles)(?:\.dev|\.beta)?\.nrfcloud\.com/([a-f0-9-]+)/(APP[^/]*)?", uris[0])
        elif fw_type == FWType.mfw:
            m = re.match(r"https://(firmware|bundles)(?:\.dev|\.beta)?\.nrfcloud\.com/([a-f0-9-]+)/(MODEM[^/]*)?", uris[0])
        else:
            m = re.match(r"https://(firmware|bundles)(?:\.dev|\.beta)?\.nrfcloud\.com/([a-f0-9-]+)/(BOOT[^/]*)?", uris[0])
        if not m:
            raise NRFCloudFOTAError(f"Unable to parse bundleId from uris: {uris}")
        if m.group(1) == "firmware":
            return m.group(3)
        return m.group(2)

    def upload_zephyr_zip(self, zip_path: str, version: str, name: str=""):
        """
        Upload zip image built by zephyr

        This adds the required 'fwversion' field to the manifest file

        :param zip_path: Path to zephyr-built zip file
        :param version: Firmware version as shown in nrfCloud UI
        :param name: Name as shown in nrfcloud UI
        :return: NRFCloud bundleId parameter"
        """
        fd = io.BytesIO()
        newz = zipfile.ZipFile(fd, "w")
        with zipfile.ZipFile(zip_path) as z:
            for i in z.namelist():
                if i == "manifest.json":
                    data = json.loads(z.read(i))
                    data["fwversion"] = version
                    if name:
                        data["name"] = name
                    newz.writestr("manifest.json", json.dumps(data))
                else:
                    newz.writestr(i, z.read(i))
        newz.close()
        fd.seek(0)
        headers = {
            "Content-Type": "application/zip"
        }
        r = self._post("/firmwares", headers=headers, data=fd.read())
        uris = r.json()["uris"]
        m = re.match(
            r"https://(firmware|bundles)(?:\.dev|\.beta)?\.nrfcloud\.com/([a-f0-9-]+)/((?:APP|MODEM|BOOT)[^/]*)?",
            uris[0]
        )
        if not m:
            raise NRFCloudFOTAError(f"Unable to parse bundleId from uris: {uris}")
        if m.group(1) == "firmware":
            return m.group(3)
        return m.group(2)

    def list_fota_jobs(self, pageLimit=10, pageNextToken=None) -> dict:
        params = {"pageLimit": pageLimit}
        if pageNextToken:
            params["pageNextToken"] = pageNextToken
        return self._get("/fota-jobs", params=params)

    def delete_fota_job(self, job_id: str):
        return self._delete(f"/fota-jobs/{job_id}")

    def cancel_fota_job(self, job_id: str):
        """
        Cancels the FOTA job specified by 'job_id'
        """
        return self._put(f"/fota-jobs/{job_id}/cancel")

    def delete_bundle(self, bundle_id: str):
        try:
            self._delete(f"/firmwares/{bundle_id}")
        except HTTPError:
            logger.warning(f"Failed to delete bundle: {bundle_id}")
            return False
        logger.info(f"Deleled bundle ID: {bundle_id}")
        return True

    def create_fota_job(self, device_id: str, bundle_id: str) -> str:
        """
        Start a FOTA update process

        :param device_id: Name as shown in nrfCloud UI
        :param bundle_id: Path to binary firmware image
        :return: nRFCloud jobId parameter"
        """
        data = json.dumps({"deviceIds": [device_id], "bundleId": bundle_id})
        return self._post("/fota-jobs", data=data).json()["jobId"]

    def get_fota_status(self, job_id: str) -> str:
        """Get status of a FOTA job

        :param job_id: Nrfcloud FOTA jobID
        :return: FOTA status string
        """
        return self._get(f"/fota-jobs/{job_id}")["status"]


    def post_fota_job(self, uuid: str, fw_id: str) -> Union[str, None]:
        """
        Posts a new FOTA job for the devices specified in the list 'uuids'

        If the job is successfully posted, i.e., a 200 status code is returned,
        then the validity of the job is checked by probing nRF Cloud a number of
        times in sleep intervals. Repeat 3 times until the job is IN_PROGRESS.

        Returns the FOTA job id if the job was successfully posted, otherwise
        'None'.
        """
        for _ in range(3):
            job_id = self.create_fota_job(uuid, fw_id)
            logger.info(f"Successfully posted FOTA job {job_id}")
            try:
                # Apply job if not automatic (with FOTA v3)
                self._post(f"/fota-jobs/{job_id}/apply")
            except HTTPError:
                pass # Do nothing if the above API call failed
            finally:
                logger.info(f"Job {job_id} is applied")
            status = "NONE"
            for _ in range(10):
                time.sleep(5)
                try:
                    status = self.get_fota_status(job_id)
                except HTTPError:
                    continue
                logger.info(f"FOTA job status: {status}")
                if status == "IN_PROGRESS":
                    return job_id
            logger.warning("Timed out while waiting for job status 'IN_PROGRESS'")
            logger.info("Cancel job, delete and retry")
            if status != "CANCELLED":
                self.cancel_fota_job(job_id)
            self.delete_fota_job(job_id)
        return None

    def cancel_incomplete_jobs(self, uuid):
        fota_jobs = self.list_fota_jobs(pageLimit=100)
        items = fota_jobs['items']
        while "pageNextToken" in fota_jobs:
            fota_jobs = self.list_fota_jobs(pageLimit=100, pageNextToken=fota_jobs["pageNextToken"])
            items = items + fota_jobs["items"]
        logger.debug("Listing jobs:")
        for job in items:
            if 'COMPLETE' in job['status']:
                continue
            if job['status'] in ["IN_PROGRESS", "QUEUED"]  and uuid in job['target']['deviceIds'][0]:
                logger.debug(f"Job {job['jobId']} not completed for device {uuid}")
                logger.info(f"Cancelling in progress job {job['jobId']}")
                try:
                    self.patch_execution_state(uuid=uuid, job_id=job['jobId'], status="CANCELLED")
                except Exception as e:
                    logger.warning(f"Failed to cancel fota job due to exception: {e}, skipping.")
                    continue

    def patch_execution_state(self, uuid: str, job_id: str, status):
        """
        Updates the execution state for the device for a given job
        """
        valid_states = [ "QUEUED", "IN_PROGRESS", "FAILED", "SUCCEEDED", "TIMED_OUT", "CANCELLED", "REJECTED", "DOWNLOADING" ]
        if status not in valid_states:
            raise NRFCloudFOTAError(f"Invalid patch value '{status}'")
        data = json.dumps({
            "status": status
        })
        return self._patch(f"/fota-job-executions/{uuid}/{job_id}", data=data)
