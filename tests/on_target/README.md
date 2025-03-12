# Assert Tracker Template on target test

## Run test locally

NOTE: The tests have been tested on Ubuntu 22.04. For details on how to install Docker please refer to the Docker documentation https://docs.docker.com/engine/install/ubuntu/

### Setup docker
```shell
docker pull ghcr.io/nordicsemiconductor/asset-tracker-template:test-docker-v1.0.0
cd <path_to_att_dir>
docker run --rm -it \
  --privileged \
  -v /dev:/dev:rw \
  -v /run/udev:/run/udev \
  -v .:/work/asset-tracker-template \
  -v /opt/setup-jlink:/opt/setup-jlink \
  ghcr.io/nordicsemiconductor/asset-tracker-template:test-docker-v1.0.0 \
  /bin/bash
cd asset-tracker-template/tests/on_target
```

### Verify nrfutil/jlink works
```shell
JLinkExe -V
nrfutil -V
```

### NRF91 tests
Precondition: thingy91x with segger fw on 53

Get device id
```shell
nrfutil device list
```

Set env
```shell
export SEGGER=<your_segger>
```

Additional fota and memfault envs
```shell
export UUID=<your_imei>
export NRFCLOUD_API_KEY=<your_nrfcloud_api_key>
```

Run desired tests, example commands
```shell
pytest -s -v -m "not slow" tests
pytest -s -v -m "not slow" tests/test_functional/test_network_reconnect.py
pytest -s -v -m "not slow" tests/test_functional/test_sampling.py
pytest -s -v -m "slow" tests/test_functional/test_fota.py::test_full_mfw_fota
```

## Test docker image version control

JLINK_VERSION=V794i
GO_VERSION=1.20.5
