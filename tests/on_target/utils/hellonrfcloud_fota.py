##########################################################################################
# Copyright (c) 2024 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import os
from typing import Union
import requests
from requests.exceptions import HTTPError
import sys
sys.path.append(os.getcwd())
from utils.logger import get_logger

logger = get_logger()

HELLO_NRF_CLOUD_URL = "https://api.hello.nordicsemi.cloud/2024-04-17"

class HelloNrfCloudFOTA():
    def __init__(self, device_id: str, fingerprint: str, timeout: int=20) -> None:
        """Initializes the class.

        :param url: Default API URL
        :param timeout: Timeout for the requests
        """
        self.url = HELLO_NRF_CLOUD_URL
        self.fingerprint = fingerprint
        self.device_id = device_id
        self.timeout = timeout

    def post_fota_job(self, type:str, bundle_id: str) -> Union[dict, None]:
        """
        Posts a new FOTA job to the specified device.

        :param type: The type of FOTA update to be sent
        :param bundle_id: The bundle ID to be sent in the payload
        :return: The JSON response from the API or None in case of error
        """
        if type in ["delta", "full"]:
            type = "modem"
        url = f"{self.url}/device/{self.device_id}/fota/{type}"

        params = {
            "fingerprint": self.fingerprint
        }

        payload = {
            "upgradePath": {
                ">=0.0.0": bundle_id
            }
        }

        try:
            response = requests.post(url, json=payload, params=params, timeout=self.timeout)
            response.raise_for_status()
            return response.json()

        except HTTPError as http_err:
            logger.error(f"HTTP error occurred: {http_err}")
        except Exception as err:
            logger.error(f"An error occurred: {err}")

        return None

    def get_fota_bundles(self) -> Union[dict, None]:
        """
        Retrieves the list of FOTA bundles available for the specified device.

        :param fingerprint: The fingerprint to be used in the request
        :return: The JSON response from the API or None in case of error
        """
        url = f"{self.url}/device/{self.device_id}/fota/bundles"

        params = {
            "fingerprint": self.fingerprint
        }

        try:
            response = requests.get(url, params=params, timeout=self.timeout)
            response.raise_for_status()
            return response.json()

        except HTTPError as http_err:
            logger.error(f"HTTP error occurred: {http_err}")
        except Exception as err:
            logger.error(f"An error occurred: {err}")

        return None

    def list_fota_jobs(self) -> Union[dict, None]:
        """
        Retrieves the list of FOTA jobs available for the specified device.

        :return: The JSON response from the API or None in case of error
        """

        url = f"{self.url}/device/{self.device_id}/fota/jobs"

        params = {
            "fingerprint": self.fingerprint
        }

        headers = {
            "Content-Type": "application/json"
        }

        try:
            response = requests.get(url, headers=headers, params=params, timeout=self.timeout)
            response.raise_for_status()
            results = response.json()
            jobs = results["jobs"]

            return jobs

        except HTTPError as http_err:
            logger.error(f"HTTP error occurred: {http_err}")
        except Exception as err:
            logger.error(f"An error occurred: {err}")

        return None

    def check_pending_jobs(self) -> Union[dict, None]:
        """
        Retrieves the list of pending FOTA jobs.

        :return: The JSON response from the API or None in case of error
        """
        jobs = self.list_fota_jobs()
        pending_jobs = [job for job in jobs if job["status"] == "IN_PROGRESS"]

        return pending_jobs


    def delete_jobs(self, jobs: list) -> None:
        """
        Deletes FOTA jobs.

        :param jobs: The list of jobs
        """

        url = f"{self.url}/device/{self.device_id}/fota/job"

        params = {
            "fingerprint": self.fingerprint
        }

        headers = {
            "Content-Type": "application/json"
        }

        for job in jobs:
            job_id = job["id"]
            job_url = f"{url}/{job_id}"
            try:
                response = requests.delete(job_url, headers=headers, params=params, timeout=self.timeout)
                response.raise_for_status()
            except HTTPError as http_err:
                logger.error(f"HTTP error occurred: {http_err}")
            except Exception as err:
                logger.error(f"An error occurred: {err}")

        return None
