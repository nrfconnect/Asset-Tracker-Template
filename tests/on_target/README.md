# Asset Tracker Template on target test

## Run test locally

Precondition: a thingy91x (with Segger firmware on the nRF5340), an nRF9151 DK or an nRF9251 DK, set with `DUT_DEVICE_TYPE`.

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

Get the probe/device serial:

```shell
nrfutil device list
```

```shell
export SEGGER=<your_jlink_serial>
export DUT_DEVICE_TYPE=thingy91x
```

Additional fota and memfault envs
```shell
export UUID=<your_imei>
export MEMFAULT_ORGANIZATION_TOKEN=<your_organization_auth_token>
export MEMFAULT_ORGANIZATION_SLUG=<your_organization_slug>
export MEMFAULT_PROJECT_SLUG=<your_project_slug>
```

The tests use an Organization Auth Token of the project. Create
one under **Admin** > **Organization Auth Tokens**, and find both slugs under **Project
Settings** > **General**. Onboarding uses the `nrfcloud` CLI from `memfault-cli`.

FOTA tests: the application, the bootloader and the modem firmware are updated with Memfault
OTA releases, see `utils/memfault_ota.py`. The tests use the [Memfault CLI](https://docs.memfault.com/docs/ci/install-memfault-cli)
(`memfault-cli`, in `requirements.txt`), and are skipped without these envs:
```shell
export MEMFAULT_ORGANIZATION_TOKEN=<your_memfault_organization_token>
export MEMFAULT_ORGANIZATION_SLUG=<your_memfault_organization_slug>
export MEMFAULT_PROJECT_SLUG=<your_memfault_project_slug>
# The cohort the DUT is in. Releases are deployed to it, so do not use a cohort of other devices.
export MEMFAULT_TEST_COHORT=<your_cohort_of_the_dut>
```

All the FOTA tests share one Memfault release per device type, which has the version of the
fota-target build (`x.x.x-fota-test-<device>-<date>`). It sorts after the `x.x.x-dev` baseline,
and each device type has its own release, so boards tested at the same time do not interfere.
A test uploads its images to the release and deploys it to the cohort, and removes the images and
deactivates the release when it is done. The CLI cannot delete an image, so the tests also call
the Memfault REST API (`DELETE .../artifacts/<id>`) with the organization token, which needs the
same role as `deploy-release` (Manager).

If a run is killed, the next test cleans up what is left. At the start of a test, it deactivates
all the `*-fota-test-<device>-*` releases of the device type that are at most
`STALE_RELEASE_DAYS` (3) days old, according to the date in the version, and deletes their images.
The test logs one line with the number of releases it checked and images it deleted. Releases
that have no device in their version, such as `x.x.x-fota-test-<date>`, are not matched, and
neither are older releases. Remove them in Memfault, or raise `STALE_RELEASE_DAYS` in
`utils/memfault_ota.py` for a one-off clean-up of older releases.

Memfault only offers a version that sorts after the one that the device reports, so the tests
start from the `x.x.x-dev` build and update to the fota-target build, and the modem component
versions of the tests sort after the version of the modem. The release-to-nightly test updates
from a release build to the fota-target build. A pre-release version sorts before the plain
release, so that test needs the Memfault project to skip the version check.

The version is read from `artifacts/fota-target-version-<device>.txt` of the fota-target build
(or set `FOTA_TARGET_VERSION`). Use memfault-cli 1.11.0 or later (`--component-version`).

The tests that resume a download disconnect the network at a given percentage, reconnect, and
trigger a poll with `att_fota poll` to resume the download.

Files of the FOTA tests, which are skipped when they are not provided:
```shell
# dfu_mcuboot.zip of a build with bootloader (fw_info) version 3, for the bootloader test.
# artifacts/dfu_mcuboot_fw_info_v3-<DUT_DEVICE_TYPE>.zip, one per board, which are checked
# in (the baseline has version 2, see app/sysbuild/mcuboot/prj.conf). To regenerate one, build
# the board with version 3, for example for nrf9151dk (thingy91x: thingy91x/nrf9151/ns):
#   west build -b nrf9151dk/nrf9151/ns -d build-bl-v3 -p --sysbuild app -- \
#       -Dmcuboot_CONFIG_FW_INFO_FIRMWARE_VERSION=3
# and copy build-bl-v3/dfu_mcuboot.zip to artifacts/dfu_mcuboot_fw_info_v3-nrf9151dk.zip.
# Use "git add -f", as artifacts/ is ignored.
# The release hex that test_app_fota_release_to_nightly starts from. By default the latest GitHub
# release is downloaded. To use a hex file of your own, set both (the version is the app version
# that the hex reports, for example 1.6.1):
export RELEASE_HEX_FILE=<path_to_release_hex>
export RELEASE_HEX_VERSION=<app_version_of_the_hex>
# The modem FOTA files, for the modem tests, in artifacts/mfw_fota:
#   mfw_nrf91x1_2.0.4.cbor
#   mfw_nrf91x1_update_from_2.0.4_to_2.0.4-FOTA-TEST.bin
#   mfw_nrf91x1_update_from_2.0.4-FOTA-TEST_to_2.0.4.bin
```

Run the tests of the helpers (they do not need hardware) with `python3 -m pytest utils`.

On macOS also set `UART_ID`: port names (`/dev/tty.usbmodem*`) don't contain the SEGGER
serial that `UART_ID` defaults to, so set it to a substring shared by your DUT's two ports
(seen in `nrfutil device list`):

```shell
export UART_ID=<substring of DUT ports, e.g. usbmodem2140>
```

### Provide firmware artifacts

The tests read firmware from `artifacts/`. In CI this folder is populated automatically, but
when running locally you must generate the merged image and copy your build output into it:

```shell
west ncs-mergehex --no-rebuild -d ../../../build
cp ../../../build/merged.hex \
   artifacts/asset-tracker-template-dev-${DUT_DEVICE_TYPE}-nrf91.hex
cp ../../../build/app/zephyr/zephyr.signed.hex \
   artifacts/asset-tracker-template-dev-${DUT_DEVICE_TYPE}-update-signed.hex
```

### Run Tests

```shell
# Use -m "not slow" to skip long-running tests (FOTA, memfault, etc.)
pytest -s -v -m "not slow" tests
pytest -s -v -m "not slow" tests/test_functional/test_network_reconnect.py
pytest -s -v -m "not slow" tests/test_functional/test_sampling.py
# Use -m "slow" to run only the long-running tests
pytest -s -v -m "slow" tests/test_functional/test_fota.py::test_full_mfw_fota
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
