# Asset Tracker Template on target test

## Run test locally

Precondition: thingy91x with segger fw on 53.

### Setup

#### Linux (Docker)

NOTE: The tests have been tested on Ubuntu 22.04. For details on how to install Docker please refer to the Docker documentation https://docs.docker.com/engine/install/ubuntu/

```shell
docker pull ghcr.io/nrfconnect/asset-tracker-template:test-docker-v1.0.2
cd <path_to_att_dir>
docker run --rm -it \
  --privileged \
  -v /dev:/dev:rw \
  -v /run/udev:/run/udev \
  -v .:/work/asset-tracker-template \
  -v /opt/setup-jlink:/opt/setup-jlink \
  ghcr.io/nrfconnect/asset-tracker-template:test-docker-v1.0.2 \
  /bin/bash
cd /work/asset-tracker-template/tests/on_target
```

Verify the toolchain:
```shell
JLinkExe -V
nrfutil -V
```

#### macOS (natively)

Docker containers on macOS can't access USB debug probes, so run the tests directly on the host.
Requires an nRF Connect SDK toolchain, `nrfutil`, and a `west`-initialized workspace.

```shell
source .venv/bin/activate     # activate workspace env
cd project/tests/on_target    # move to this folder
pip install -r requirements.txt
```

### Set env

To run tests locally, a number of environment variables are required. To persist these settings,
you can place them in a `.envrc` file at your West workspace root.

Get the probe/device serial:

```shell
nrfutil device list
```

```shell
export SEGGER=<your_jlink_serial>
export DUT_DEVICE_TYPE=thingy91x
```

Devices register with nRF Cloud using the modem's device UUID, so you can get the modem UUID via the AT interface:

```plaintext
uart~$ at AT%DEVICEUUID
```

```shell
export UUID=<your_device_uuid>
```

Then, get your nRF Cloud credentials:

- [API Key](https://docs.nrfcloud.com/docs/legacy-nrfcloud/tokens-and-keys#api-key)
- [Organization Token](https://app.memfault.com/organizations/-/settings/auth-tokens)
- [Organization Slug and Project Slug](https://app.memfault.com/organizations/-/projects/-/settings)
- (Optional) Create a [Modem Project](https://docs.nrfcloud.com/docs/mcu/nrf-modem-fota#step-1-create-a-modem-firmware-project)

```shell
export NRFCLOUD_API_KEY=<your_nrfcloud_api_key>
export MEMFAULT_ORGANIZATION_TOKEN=<your_org_token>
export MEMFAULT_ORGANIZATION_SLUG=<your_org_slug>
export MEMFAULT_PROJECT_SLUG=<your_app_project_slug>

# Optional: defaults to "default" cohort
export MEMFAULT_OTA_COHORT=<cohort>
# Optional: defaults to DUT_DEVICE_TYPE
export MEMFAULT_HW_VERSION=<hardware_version>
# Optional: delta modem FOTA uses a separate OTA project (test skips if unset).
export MEMFAULT_MODEM_PROJECT_SLUG=<your_modem_project_slug>
export MEMFAULT_MODEM_ORGANIZATION_TOKEN=<your_modem_org_token>
export MEMFAULT_MODEM_ORGANIZATION_SLUG=<your_modem_org_slug>
```

On macOS also set `UART_ID`: port names (`/dev/tty.usbmodem*`) don't contain the SEGGER
serial that `UART_ID` defaults to, so set it to a substring shared by your DUT's two ports
(seen in `nrfutil device list`):

```shell
export UART_ID=<substring of DUT ports, e.g. usbmodem2140>
```

### Provide firmware artifacts

The tests read firmware from `artifacts/`. In CI this folder is populated automatically, but
when running locally you must copy your build output into it:

```shell
cp ../../../build/merged.hex \
   artifacts/asset-tracker-template-dev-${DUT_DEVICE_TYPE}-nrf91.hex
```

`test_delta_mfw_fota` additionally needs Nordic's `mfw_nrf91x1_2.0.3` and `mfw_nrf91x1_2.0.4`
release zips in `artifacts/` (used to flash the modem to a known 2.0.3 baseline and to restore it
to 2.0.4 afterward), plus the `mfw_nrf91x1_update_from_2.0.3_to_2.0.4.bin` delta bundled inside the
2.0.4 zip:

```shell
cp <path_to>/mfw_nrf91x1_2.0.3.zip artifacts/
cp <path_to>/mfw_nrf91x1_2.0.4.zip artifacts/
cp <path_to>/mfw_nrf91x1_2.0.4/mfw_nrf91x1_update_from_2.0.3_to_2.0.4.bin artifacts/
```

`test_app_fota` needs an app update image built at `PATCHLEVEL+1` (CI builds this in `build.yml`'s
app-fota-update steps; locally, bump `PATCHLEVEL` in `app/VERSION` by 1, build, then restore it):

```shell
# with PATCHLEVEL bumped by 1 in app/VERSION
west build -p -b thingy91x/nrf9151/ns --sysbuild
cp build/app/zephyr/zephyr.signed.bin \
   artifacts/asset-tracker-template-<bumped-version>-app-fota-update-${DUT_DEVICE_TYPE}-nrf91-update-signed.bin
# restore app/VERSION afterward
```

`test_delta_mfw_fota` needs an app image built with the modem project key compiled in (CI builds
this in `build.yml`'s modem-fota-baseline steps):

```shell
west build -p -b thingy91x/nrf9151/ns --sysbuild -- \
  -Dapp_CONFIG_MEMFAULT_FOTA_MODEM_PROJECT_KEY=\"<your_modem_fota_project_key>\"
cp build/merged.hex \
   artifacts/asset-tracker-template-<version>-modem-fota-baseline-${DUT_DEVICE_TYPE}-nrf91.hex
```

### Run Tests

```shell
# Use -m "not slow" to skip long-running tests (FOTA, memfault, etc.)
pytest -s -v -m "not slow" tests
pytest -s -v -m "not slow" tests/test_functional/test_network_reconnect.py
pytest -s -v -m "not slow" tests/test_functional/test_sampling.py
# Use -m "slow" to run only the long-running tests
pytest -s -v -m "slow" tests/test_functional/test_fota.py::test_app_fota
pytest -s -v -m "slow" tests/test_functional/test_fota.py::test_delta_mfw_fota
```

## Run tests in CI

Two GitHub Actions workflows drive on-target testing:

- **`build-and-target-test.yml`** ("Build and Test") builds firmware and then runs the on-target
  tests against it, in one pipeline. This is what you want in most cases.
- **`target-test.yml`** ("Target tests") only runs the tests, against artifacts from a previous
  build run (it takes an `artifact_run_id`). Use it to re-run tests without rebuilding.

### Exercising the FOTA CI jobs

The FOTA tests (`test_app_fota`, `test_delta_mfw_fota`) are `@pytest.mark.slow` and depend on
dedicated firmware images that are **only built when nightly tests are enabled**:

- `*-app-fota-update-*` — the app image at `PATCHLEVEL+1` that `test_app_fota` updates to.
- `*-modem-fota-baseline-*` — the app image with `CONFIG_MEMFAULT_FOTA_MODEM_PROJECT_KEY`
  compiled in that `test_delta_mfw_fota` flashes before the modem update.

Both are produced by `build.yml`'s `build_<board>_app_fota_update` /
`build_<board>_modem_fota_baseline` steps, which `build-and-target-test.yml` only enables when
`run_nightly_tests` is true (nightly schedule, or a manual dispatch with the flag set). An ordinary
PR/push build does **not** produce them, so running `target-test.yml` against such a build skips or
fails the FOTA tests at the fixture stage.

To exercise the FOTA jobs, dispatch the combined pipeline with nightly tests enabled:

```shell
gh workflow run build-and-target-test.yml \
  --ref <your-branch> \
  -f device_thingy91x=true \
  -f run_nightly_tests=true
```

This builds the FOTA images and runs the full suite (including the slow FOTA tests) against them.
`test_delta_mfw_fota` additionally requires the `MEMFAULT_FOTA_MODEM_PROJECT_KEY` repository/
organization secret (used at build time to compile the key into the modem-fota-baseline image) and
the `MEMFAULT_MODEM_*` variables (see [Set env](#set-env)); it skips if the modem OTA project is
not configured.

To re-run just the tests against an existing nightly build's artifacts, grab that
build's run ID and firmware version and dispatch the test-only workflow:

```shell
gh workflow run target-test.yml \
  --ref <your-branch> \
  -f artifact_run_id=<build_run_id> \
  -f artifact_fw_version=<version_printed_by_the_build> \
  -f devices="thingy91x" \
  -f run_nightly_tests=true \
  -f pytest_args='-k "test_app_fota or test_delta_mfw_fota"'
```

## Test docker image version control

JLINK_VERSION=V794i

GO_VERSION=1.20.5


## Experimental: docker etb-decode
```shell
docker pull ghcr.io/dematteisgiacomo/etb_decoder:latest
docker run --rm -it \
  ghcr.io/dematteisgiacomo/etb_decoder:latest \
  nrfutil toolchain-manager launch --shell
```

then in the docker:
```shell
etb-decode -h
```

try example files:
```shell
etb-decode -d etb_trace_decoder/example_etb_coredump.elf -s etb_trace_decoder/example_elf_file.elf -o decoded.txt
```

mount your own files and try it out
