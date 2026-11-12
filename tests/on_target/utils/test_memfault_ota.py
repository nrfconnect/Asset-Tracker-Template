##########################################################################################
# Copyright (c) 2026 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import json
import os
import pathlib
import subprocess
import sys
import urllib.error
import zipfile
from unittest.mock import Mock

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from utils.memfault_ota import (  # noqa: E402
    MemfaultOTA,
    MemfaultOTAConfigError,
    MemfaultOTAError,
    OTAPayload,
    extract_dfu_zip,
)

TOKEN = "secret-token"
RELEASE = "1.2.3-fota-test-20261109"


class FakeResponse:
    def __init__(self, body=b""):
        self._body = body

    def read(self):
        return self._body

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False


class FakeAPI:
    """Stands in for the REST API: keeps the artifacts of the release."""

    def __init__(self, artifacts=None, fail_delete=False):
        self.artifacts = list(artifacts or [])
        self.fail_delete = fail_delete
        self.requests = []

    def __call__(self, request):
        self.requests.append(request)
        path = request.full_url.split("/projects/proj/")[1]
        assert request.get_header("Authorization") == f"Bearer {TOKEN}"
        if request.get_method() == "GET":
            assert path == f"releases/{RELEASE}"
            return FakeResponse(json.dumps({"artifacts": self.artifacts}).encode())
        if self.fail_delete:
            raise urllib.error.HTTPError(request.full_url, 403, "Forbidden", {}, None)
        artifact_id = int(path.split("/")[1])
        self.artifacts = [a for a in self.artifacts if a["id"] != artifact_id]
        return FakeResponse()

    def methods(self):
        return [(r.get_method(), r.full_url.split("/projects/proj/")[1]) for r in self.requests]


def make_ota(runner=None, api=None):
    runner = runner or Mock(return_value=Mock(stdout=""))
    api = api or FakeAPI()
    ota = MemfaultOTA(TOKEN, "org", "proj", "test-cohort", "hw", RELEASE, runner=runner,
                      opener=api)
    return ota, runner


def memfault_args(runner, call=-1):
    """The arguments of a memfault command, without the global options."""
    command = runner.call_args_list[call].args[0]
    assert command[:5] == ["memfault", "--org", "org", "--project", "proj"]
    assert TOKEN not in command
    return command[5:]


def commands(runner):
    return [memfault_args(runner, i)[0] + (" --deactivate" if "--deactivate" in memfault_args(
        runner, i) else "") for i in range(runner.call_count)]


def test_upload_primary_payload():
    ota, runner = make_ota()

    ota.upload_payload(OTAPayload("app.bin", "app"))

    assert memfault_args(runner) == [
        "upload-ota-payload", "--hardware-version", "hw", "--software-type", "app",
        "--software-version", RELEASE, "app.bin"]


def test_upload_component_payload():
    ota, runner = make_ota()

    ota.upload_payload(OTAPayload("mcuboot.bin", "mcuboot-s0", component_version="3"))

    args = memfault_args(runner)
    assert args[args.index("--software-type") + 1] == "mcuboot-s0"
    assert args[args.index("--software-version") + 1] == RELEASE
    assert args[args.index("--component-version") + 1] == "3"
    assert "--component-base-version" not in args
    assert args[-1] == "mcuboot.bin"


def test_upload_component_delta_payload():
    ota, runner = make_ota()

    ota.upload_payload(OTAPayload("delta.bin", "mfw", component_version="mfw_2",
                                  component_base_version="mfw_1"))

    args = memfault_args(runner)
    assert args[args.index("--component-version") + 1] == "mfw_2"
    assert args[args.index("--component-base-version") + 1] == "mfw_1"


def test_delta_payload_needs_the_resulting_component_version():
    with pytest.raises(ValueError):
        OTAPayload("delta.bin", "mfw", component_base_version="mfw_1")


def test_deploy_and_deactivate_use_the_test_cohort():
    ota, runner = make_ota()

    ota.deploy_release()
    ota.deactivate_release()

    assert memfault_args(runner, 0) == [
        "deploy-release", "--release-version", RELEASE, "--cohort", "test-cohort"]
    assert memfault_args(runner, 1) == [
        "deploy-release", "--release-version", RELEASE, "--cohort", "test-cohort", "--deactivate"]


def test_release_uploads_deploys_then_deactivates_and_deletes():
    api = FakeAPI([{"id": 1, "software_type": "app"}])
    ota, runner = make_ota(api=api)
    payloads = [OTAPayload("a.bin", "app"), OTAPayload("m.bin", "mfw", component_version="v")]

    with ota.release(payloads) as version:
        assert version == RELEASE
        # The image of an earlier run is gone before the new ones are uploaded
        assert api.artifacts == []
        assert commands(runner) == [
            "deploy-release --deactivate", "upload-ota-payload", "upload-ota-payload",
            "deploy-release"]
        api.artifacts = [{"id": 2}, {"id": 3}]

    assert commands(runner)[-1] == "deploy-release --deactivate"
    assert api.artifacts == []


def test_release_is_cleaned_up_when_the_test_fails():
    api = FakeAPI()
    ota, runner = make_ota(api=api)

    def failing_test():
        with ota.release([OTAPayload("a.bin", "app")]):
            api.artifacts = [{"id": 5}]
            raise RuntimeError("test failed")

    with pytest.raises(RuntimeError, match="test failed"):
        failing_test()

    assert memfault_args(runner)[-1] == "--deactivate"
    assert api.artifacts == []


def test_failing_clean_up_does_not_hide_the_result():
    api = FakeAPI()

    def runner(command, **kwargs):
        if "--deactivate" in command:
            raise subprocess.CalledProcessError(1, command, stderr="boom")
        return Mock(stdout="")

    ota, _ = make_ota(runner, api)

    with ota.release([OTAPayload("a.bin", "app")]):
        api.artifacts = [{"id": 5}]
        api.fail_delete = True


def test_stale_images_that_cannot_be_deleted_fail_the_test():
    ota, _ = make_ota(api=FakeAPI([{"id": 1}], fail_delete=True))

    try:
        with ota.release([OTAPayload("a.bin", "app")]):
            pytest.fail("The release must not be used")
    except MemfaultOTAError as e:
        assert "403" in str(e)
    else:
        pytest.fail("The failing delete must raise")


def test_release_does_not_deploy_when_upload_fails_and_still_cleans_up():
    api = FakeAPI()

    def runner(command, **kwargs):
        if "upload-ota-payload" in command:
            api.artifacts = [{"id": 9}]
            raise subprocess.CalledProcessError(2, command, stderr="upload rejected")
        return Mock(stdout="")

    ota, _ = make_ota(runner, api)

    try:
        with ota.release([OTAPayload("a.bin", "app")]):
            pytest.fail("The release must not be used")
    except MemfaultOTAError as e:
        assert "upload rejected" in str(e)
    else:
        pytest.fail("The failing upload must raise")

    assert api.artifacts == []


def test_release_that_does_not_exist_has_no_payloads():
    def opener(request):
        raise urllib.error.HTTPError(request.full_url, 404, "Not Found", {}, None)

    ota = MemfaultOTA(TOKEN, "org", "proj", "c", "hw", RELEASE, opener=opener)

    assert ota.list_payloads() == []


def test_api_error_does_not_contain_the_token():
    def opener(request):
        raise urllib.error.URLError("unreachable")

    ota = MemfaultOTA(TOKEN, "org", "proj", "c", "hw", RELEASE, opener=opener)

    with pytest.raises(MemfaultOTAError) as error:
        ota.delete_payload(1)

    assert TOKEN not in str(error.value)
    assert TOKEN not in repr(error.value.__cause__)


def test_error_does_not_contain_the_token():
    def runner(command, **kwargs):
        raise subprocess.CalledProcessError(1, command, stderr="denied")

    ota, _ = make_ota(runner)

    with pytest.raises(MemfaultOTAError) as error:
        ota.deploy_release()

    assert TOKEN not in str(error.value)


def test_token_is_masked_in_repr_and_subprocess_errors():
    def runner(command, **kwargs):
        raise subprocess.CalledProcessError(2, command, stderr="bad option")

    ota, _ = make_ota(runner)
    with pytest.raises(MemfaultOTAError) as excinfo:
        ota.deploy_release()

    chained = repr(excinfo.value.__cause__) + str(excinfo.value.__cause__)
    assert TOKEN not in chained
    assert TOKEN not in repr(ota.__dict__)


def test_token_is_passed_in_the_environment_not_on_the_command_line():
    ota, runner = make_ota()
    ota.deploy_release()

    env = runner.call_args.kwargs["env"]
    assert str.__str__(env["MEMFAULT_ORG_TOKEN"]) == TOKEN
    assert TOKEN not in repr(env)
    assert all(TOKEN not in arg for arg in runner.call_args.args[0])


def test_missing_cli():
    def runner(command, **kwargs):
        raise FileNotFoundError("memfault")

    ota, _ = make_ota(runner)

    with pytest.raises(MemfaultOTAError, match="not installed"):
        ota.deploy_release()


def test_with_release_version_keeps_the_settings():
    runner = Mock(return_value=Mock(stdout=""))
    ota, _ = make_ota(runner)

    other = ota.with_release_version("2.0.0")
    other.deploy_release()

    assert (other.cohort, other.hardware_version, other.release_version) == (
        "test-cohort", "hw", "2.0.0")
    assert memfault_args(runner) == [
        "deploy-release", "--release-version", "2.0.0", "--cohort", "test-cohort"]
    assert ota.release_version == RELEASE


def test_from_env():
    ota = MemfaultOTA.from_env(RELEASE, {
        "MEMFAULT_ORGANIZATION_TOKEN": "t", "MEMFAULT_ORGANIZATION_SLUG": "o",
        "MEMFAULT_PROJECT_SLUG": "p", "MEMFAULT_TEST_COHORT": "c",
        "DUT_DEVICE_TYPE": "h"})

    assert (ota.cohort, ota.hardware_version) == ("c", "h")


def test_from_env_requires_an_explicit_cohort():
    environ = {
        "MEMFAULT_ORGANIZATION_TOKEN": "t", "MEMFAULT_ORGANIZATION_SLUG": "o",
        "MEMFAULT_PROJECT_SLUG": "p", "DUT_DEVICE_TYPE": "h"}

    with pytest.raises(MemfaultOTAConfigError, match="MEMFAULT_TEST_COHORT"):
        MemfaultOTA.from_env(RELEASE, environ)


def test_extract_dfu_zip(tmp_path):
    archive = tmp_path / "dfu.zip"
    manifest = {"files": [
        {"file": "boot_s0.bin", "slot": "0", "version_B0": "3"},
        {"file": "boot_s1.bin", "slot": "1", "version_B0": "3"}]}
    with zipfile.ZipFile(archive, "w") as z:
        z.writestr("manifest.json", json.dumps(manifest))
        z.writestr("boot_s0.bin", b"s0")
        z.writestr("boot_s1.bin", b"s1")

    entries = extract_dfu_zip(str(archive), str(tmp_path / "out"))

    assert [(e["slot"], e["version_B0"]) for e in entries] == [("0", "3"), ("1", "3")]
    assert pathlib.Path(entries[0]["path"]).read_bytes() == b"s0"
    assert pathlib.Path(entries[1]["path"]).read_bytes() == b"s1"


def test_extract_dfu_zip_removes_files_of_an_earlier_run(tmp_path):
    out = tmp_path / "out"
    out.mkdir()
    (out / "old.bin").write_bytes(b"old")
    (out / "boot_s0.bin").write_bytes(b"stale")
    archive = tmp_path / "dfu.zip"
    with zipfile.ZipFile(archive, "w") as z:
        z.writestr("manifest.json", json.dumps({"files": [{"file": "boot_s0.bin"}]}))
        z.writestr("boot_s0.bin", b"new")

    entries = extract_dfu_zip(str(archive), str(out))

    assert sorted(os.listdir(out)) == ["boot_s0.bin"]
    assert pathlib.Path(entries[0]["path"]).read_bytes() == b"new"


class FakeReleasesAPI:
    """Stands in for the REST API with several releases, and pages of two."""

    def __init__(self, releases):
        self.releases = releases
        self.requests = []

    def __call__(self, request):
        self.requests.append(request)
        path = request.full_url.split("/projects/proj/")[1]
        if request.get_method() == "DELETE":
            return FakeResponse()
        if path.startswith("releases?"):
            page = int(path.split("page=")[1].split("&")[0])
            chunk = [{"version": v} for v in self.releases[(page - 1) * 2:page * 2]]
            paging = {"page": page, "page_count": (len(self.releases) + 1) // 2}
            return FakeResponse(json.dumps({"data": chunk, "paging": paging}).encode())
        return FakeResponse(json.dumps({"artifacts": [{"id": 7}]}).encode())


def test_clean_up_stale_releases_of_the_device_only():
    versions = ["1.0.0", "1.6.1-fota-test-hw-20261116", "1.6.1-fota-test-other-20261116",
                "1.6.2-fota-test-hw-20261117", "1.6.1-fota-test-20261113"]
    api = FakeReleasesAPI(versions)
    runner = Mock(return_value=Mock(stdout=""))
    ota = MemfaultOTA(TOKEN, "org", "proj", "test-cohort", "hw", RELEASE, runner=runner, opener=api)

    ota.clean_up_stale_releases(max_age_days=3650)

    deactivated = [memfault_args(runner, i)[2] for i in range(runner.call_count)]
    assert deactivated == ["1.6.1-fota-test-hw-20261116", "1.6.2-fota-test-hw-20261117"]
    deleted = [r.full_url for r in api.requests if r.get_method() == "DELETE"]
    assert len(deleted) == 2


def test_clean_up_stale_releases_does_not_raise_when_listing_fails():
    def failing(request):
        raise urllib.error.HTTPError(request.full_url, 403, "Forbidden", {}, None)

    ota = MemfaultOTA(TOKEN, "org", "proj", "test-cohort", "hw", RELEASE,
                      runner=Mock(), opener=failing)

    ota.clean_up_stale_releases()


def test_clean_up_stale_releases_only_looks_at_the_last_days():
    from datetime import datetime, timedelta, timezone
    today = datetime.now(timezone.utc).date()
    recent = f"1.6.1-fota-test-hw-{(today - timedelta(days=1)).strftime('%Y%m%d')}"
    old = f"1.6.1-fota-test-hw-{(today - timedelta(days=30)).strftime('%Y%m%d')}"
    api = FakeReleasesAPI([recent, old])
    runner = Mock(return_value=Mock(stdout=""))
    ota = MemfaultOTA(TOKEN, "org", "proj", "test-cohort", "hw", RELEASE, runner=runner, opener=api)

    ota.clean_up_stale_releases(max_age_days=3)
    assert [memfault_args(runner, i)[2] for i in range(runner.call_count)] == [recent]

    runner.reset_mock()
    ota.clean_up_stale_releases(max_age_days=3650)
    assert [memfault_args(runner, i)[2] for i in range(runner.call_count)] == [recent, old]
