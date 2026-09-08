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
export NRFCLOUD_API_KEY=<your_nrfcloud_api_key>
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
