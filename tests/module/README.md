# Assert Tracker Template Unit Tests on native sim

## Run tests locally

### Setup docker
```shell
cd <path_to_att_dir>
docker run --rm -it \
  --privileged \
  -e BUILD_WRAPPER_OUT_DIR=build_wrapper_output_directory \
  -e CMAKE_PREFIX_PATH=/opt/toolchains \
  -v .:/work/asset-tracker-template \
  ghcr.io/zephyrproject-rtos/ci:v0.27.4 \
  /bin/bash
```

### Setup Commmands
```shell
cd work/asset-tracker-template/
west init -l .
west update -o=--depth=1 -n

pip install -r ../nrf/scripts/requirements-build.txt
apt-get update && apt install -y curl ruby-full
```

###  Run tests with Address Sanitizer, Leak Sanitizer and Undefined behaviour sanitizer
```shell
west twister -T tests/ -C --coverage-platform=native_sim -v --inline-logs --integration --enable-asan --enable-lsan --enable-ubsan
```

###  Run tests with Valgrind
```shell
west twister -T tests/ -C --coverage-platform=native_sim -v --inline-logs --integration --enable-valgrind
```
