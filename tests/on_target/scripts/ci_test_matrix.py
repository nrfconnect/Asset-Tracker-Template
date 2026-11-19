#!/usr/bin/env python3
##########################################################################################
# Copyright (c) 2026 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

"""Build the GitHub Actions job matrix for the on-target tests: one job per test function.

The tests are collected with pytest (no hardware needed), so the matrix follows the test
code. Run from tests/on_target. Writes ``matrix`` (JSON) and ``count`` to $GITHUB_OUTPUT,
and a table of the planned jobs to $GITHUB_STEP_SUMMARY.

Example:
    python3 scripts/ci_test_matrix.py --devices "thingy91x nrf9151dk" --nightly false
"""

import argparse
import json
import os
import re
import shlex
import sys
from pathlib import Path

import pytest

# Devices that run a dedicated test directory instead of --pytest-path
DEVICE_TEST_PATHS = {
    "ppk_thingy91x": "tests/test_ppk",
    "gnss_nrf9151dk": "tests/test_gnss",
    "prov_thingy91x": "tests/test_provisioning",
    "prov_nrf9251dk": "tests/test_provisioning",
}

# Characters not allowed in artifact names
ARTIFACT_NAME_INVALID = re.compile(r'[":<>|*?\\/\r\n]')


class _Collector:
    """pytest plugin that records the selected items after -m/-k deselection."""

    def __init__(self):
        self.tests = []

    def pytest_collection_finish(self, session):
        for item in session.items:
            path = Path(os.path.relpath(item.path))
            name = item.nodeid.split("::", 1)[1]
            self.tests.append((path, name))


def _is_under(path, target):
    target = Path(target)
    return path == target or target in path.parents


def _collect(paths, marker, extra_args):
    collector = _Collector()
    args = ["--collect-only", "-q", "-p", "no:cacheprovider", *paths, *extra_args]
    if marker:
        args += ["-m", marker]

    rc = pytest.main(args, plugins=[collector])
    # 5: nothing selected, which is not an error here
    if rc not in (pytest.ExitCode.OK, pytest.ExitCode.NO_TESTS_COLLECTED):
        sys.exit(f"pytest collection failed (exit code {int(rc)})")

    return collector.tests


def build_matrix(devices, nightly, pytest_path, pytest_args):
    device_paths = {device: DEVICE_TEST_PATHS.get(device, pytest_path) for device in devices}
    marker = None if nightly else "not slow"
    tests = _collect(sorted(set(device_paths.values())), marker, shlex.split(pytest_args))

    include = []
    for device, target in device_paths.items():
        device_tests = [(path, name) for path, name in tests if _is_under(path, target)]
        names = [name for _, name in device_tests]
        for path, name in device_tests:
            test_name = name if names.count(name) == 1 else f"{path.stem}::{name}"
            include.append({
                "device": device,
                "name": f"{device} - {test_name}",
                "test": f"{path.as_posix()}::{name}",
                "artifact_name": ARTIFACT_NAME_INVALID.sub("_", f"{device}-{test_name}"),
            })

    return include


def _write_outputs(include):
    matrix = json.dumps({"include": include}, separators=(",", ":"))
    lines = [f"matrix={matrix}", f"count={len(include)}"]
    if github_output := os.getenv("GITHUB_OUTPUT"):
        with open(github_output, "a", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
    else:
        print("\n".join(lines))

    if summary := os.getenv("GITHUB_STEP_SUMMARY"):
        with open(summary, "a", encoding="utf-8") as f:
            f.write(f"### Planned target tests ({len(include)})\n\n| Device | Test |\n|---|---|\n")
            for entry in include:
                f.write(f"| {entry['device']} | `{entry['test']}` |\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     allow_abbrev=False)
    parser.add_argument("--devices", required=True,
                        help="Space separated list of devices, e.g. 'thingy91x nrf9151dk'")
    parser.add_argument("--nightly", default="false", choices=["true", "false"],
                        help="Include tests marked slow")
    parser.add_argument("--pytest-path", default="tests/test_functional",
                        help="Test path for devices without a dedicated test directory")
    parser.add_argument("--pytest-args", default="",
                        help="Extra pytest arguments, shell-quoted (e.g. -k \"test_fota\")")
    args = parser.parse_args()

    devices = args.devices.split()
    if not devices:
        sys.exit("No devices given")

    include = build_matrix(devices, args.nightly == "true", args.pytest_path, args.pytest_args)
    for entry in include:
        print(f"{entry['device']:<16} {entry['test']}")
    _write_outputs(include)


if __name__ == "__main__":
    main()
