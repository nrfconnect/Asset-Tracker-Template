##########################################################################################
# Copyright (c) 2026 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

"""Memfault OTA releases for the FOTA tests, using the Memfault CLI (memfault-cli).

The application and the other OTA components (modem firmware and the bootloader slot images) are
software types of one Memfault project. A release has a release version, which is the software
version of the primary software type (the application). The other software types are components
of the release, each with a component version that the device reports as its current version.
A component can be a full image, or a delta from a base component version.

All the FOTA tests share one release, with the version of the FOTA target build. A test uploads its
images to that release and deploys it, and removes the images and deactivates the release when it
is done. The CLI cannot delete an image, so that uses the REST API of Memfault.
"""

import json
import os
import subprocess
import urllib.error
import urllib.request
import zipfile
from contextlib import contextmanager
from dataclasses import dataclass
from typing import Callable, Optional

from utils.logger import get_logger

logger = get_logger()

ENV_ORG_TOKEN = "MEMFAULT_ORGANIZATION_TOKEN"
# The variable that the memfault CLI reads for --org-token (click auto_envvar_prefix MEMFAULT)
ENV_CLI_ORG_TOKEN = "MEMFAULT_ORG_TOKEN"
ENV_ORG = "MEMFAULT_ORGANIZATION_SLUG"
ENV_PROJECT = "MEMFAULT_PROJECT_SLUG"
# The cohort the DUT is in. There is no default, as deploying to a shared cohort, such as
# "default", would update other devices in the project.
ENV_COHORT = "MEMFAULT_TEST_COHORT"
# The device type of the DUT, which is also the hardware version it reports to Memfault
ENV_DEVICE_TYPE = "DUT_DEVICE_TYPE"

API_URL = "https://api.memfault.com/api/v0"
API_TIMEOUT_SECONDS = 30

REQUIRED_ENV = (ENV_ORG_TOKEN, ENV_ORG, ENV_PROJECT, ENV_COHORT, ENV_DEVICE_TYPE)


class _Secret(str):
    """A string that does not show its value in repr().

    The CLI gets the real value, but tracebacks (including pytest's showlocals) and the text of
    subprocess errors, which use repr() of the command, show a mask.
    """

    def __repr__(self) -> str:
        return "'***'"


class MemfaultOTAError(Exception):
    """A Memfault CLI command or API request failed."""


class MemfaultOTAConfigError(Exception):
    """The environment is missing the Memfault settings."""


@dataclass
class OTAPayload:
    """An image of a release.

    Attributes:
        path: The image file.
        software_type: The Memfault software type, for example "app" or "mfw".
        component_version: The version of a component, as reported by the device. Not set for the
            primary software type, which has the release version.
        component_base_version: For a delta component, the component version the delta applies to.
    """

    path: str
    software_type: str
    component_version: Optional[str] = None
    component_base_version: Optional[str] = None

    def __post_init__(self):
        if self.component_base_version and not self.component_version:
            raise ValueError("A component delta needs the component version it results in")


def _urlopen(request: urllib.request.Request):
    return urllib.request.urlopen(request, timeout=API_TIMEOUT_SECONDS)


def extract_dfu_zip(zip_path: str, dest_dir: str) -> list:
    """Extract a DFU zip of the build, and return the files of its manifest.

    Each returned entry is the manifest entry of the file (with its slot, version and so on)
    and the path of the extracted file in "path".
    """
    os.makedirs(dest_dir, exist_ok=True)
    with zipfile.ZipFile(zip_path) as archive:
        manifest = json.loads(archive.read("manifest.json"))
        entries = []
        for entry in manifest["files"]:
            archive.extract(entry["file"], dest_dir)
            entries.append({**entry, "path": os.path.join(dest_dir, entry["file"])})

    return entries


class MemfaultOTA:
    def __init__(
        self,
        org_token: str,
        org: str,
        project: str,
        cohort: str,
        hardware_version: str,
        release_version: str,
        runner: Callable = subprocess.run,
        opener: Callable = _urlopen,
    ):
        self._org_token = _Secret(org_token)
        self._org = org
        self._project = project
        self.cohort = cohort
        self.hardware_version = hardware_version
        self.release_version = release_version
        self._runner = runner
        self._opener = opener

    @classmethod
    def from_env(cls, release_version: str, environ=os.environ) -> "MemfaultOTA":
        missing = [name for name in REQUIRED_ENV if not environ.get(name)]
        if missing:
            raise MemfaultOTAConfigError(
                f"Environment variable(s) not set: {', '.join(missing)}")

        return cls(
            org_token=environ[ENV_ORG_TOKEN],
            org=environ[ENV_ORG],
            project=environ[ENV_PROJECT],
            cohort=environ[ENV_COHORT],
            hardware_version=environ[ENV_DEVICE_TYPE],
            release_version=release_version,
        )

    def _run(self, *args: str) -> str:
        command = ["memfault", "--org", self._org, "--project", self._project, *args]
        # The token goes in the environment of the CLI, not on its command line, so it is not
        # visible in the process list or in the command of an error. MEMFAULT_ORG_TOKEN is the
        # variable that the CLI reads for --org-token.
        env = {**os.environ, ENV_CLI_ORG_TOKEN: self._org_token}
        logger.info("memfault " + " ".join(args))
        try:
            result = self._runner(
                command, check=True, capture_output=True, text=True, env=env)
        except subprocess.CalledProcessError as e:
            raise MemfaultOTAError(
                f"memfault {args[0]} failed ({e.returncode}): {e.stderr or e.stdout}") from e
        except FileNotFoundError as e:
            raise MemfaultOTAError(
                "The memfault CLI is not installed (pip install memfault-cli)") from e

        return result.stdout

    def _api(self, method: str, path: str):
        """Send a request to the REST API of the project. Returns the decoded JSON, or None."""
        url = f"{API_URL}/organizations/{self._org}/projects/{self._project}/{path}"
        request = urllib.request.Request(
            url, method=method, headers={"Authorization": f"Bearer {self._org_token}"})
        logger.info(f"memfault API {method} {path}")
        try:
            with self._opener(request) as response:
                body = response.read()
        except urllib.error.HTTPError as e:
            raise MemfaultOTAError(f"{method} {path} failed ({e.code}): {e.reason}") from e
        except urllib.error.URLError as e:
            raise MemfaultOTAError(f"{method} {path} failed: {e.reason}") from e

        return json.loads(body) if body else None

    def list_payloads(self) -> list:
        """The images of the release, as dicts with the "id" of each. Empty if there is no release."""
        try:
            body = self._api("GET", f"releases/{self.release_version}")
        except MemfaultOTAError as e:
            if isinstance(e.__cause__, urllib.error.HTTPError) and e.__cause__.code == 404:
                return []
            raise

        return (body.get("data", body) or {}).get("artifacts", [])

    def delete_payload(self, payload_id: int) -> None:
        self._api("DELETE", f"artifacts/{payload_id}")

    def delete_payloads(self) -> None:
        """Delete all the images of the release. The release must not be deployed."""
        for payload in self.list_payloads():
            self.delete_payload(payload["id"])

    def upload_payload(self, payload: OTAPayload) -> None:
        """Upload an image to the release."""
        args = [
            "upload-ota-payload",
            "--hardware-version", self.hardware_version,
            "--software-type", payload.software_type,
            "--software-version", self.release_version,
        ]
        if payload.component_version:
            args += ["--component-version", payload.component_version]
        if payload.component_base_version:
            args += ["--component-base-version", payload.component_base_version]
        args.append(payload.path)

        self._run(*args)

    def deploy_release(self) -> None:
        self._run("deploy-release", "--release-version", self.release_version,
                  "--cohort", self.cohort)

    def deactivate_release(self) -> None:
        self._run("deploy-release", "--release-version", self.release_version,
                  "--cohort", self.cohort, "--deactivate")

    def _clean_up(self) -> None:
        """Deactivate the release and delete its images. Failures are logged, not raised."""
        for step in (self.deactivate_release, self.delete_payloads):
            try:
                step()
            except MemfaultOTAError as e:
                logger.warning(f"Memfault release {self.release_version} clean-up failed: {e}")

    @contextmanager
    def release(self, payloads: list):
        """Make the release contain only the given images, and deploy it to the test cohort.

        Images left by an earlier run are removed first. On exit the release is deactivated and
        the images are deleted, also when the test fails. Yields the release version.
        """
        try:
            self.deactivate_release()
        except MemfaultOTAError as e:
            logger.info(f"Release {self.release_version} was not active: {e}")
        self.delete_payloads()
        try:
            for payload in payloads:
                self.upload_payload(payload)

            self.deploy_release()
            yield self.release_version
        finally:
            self._clean_up()
