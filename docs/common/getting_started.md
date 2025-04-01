# Getting Started

Before getting started, make sure you have a proper nRF Connect SDK development environment. Follow the official [Getting started guide](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/installation.html)

## Supported boards:
```
thingy91x/nrf9151/ns
nrf9151dk/nrf9151/ns
```

## Provision device to nrfcloud
Follow these steps to provision your device with [nRF Cloud Provisioning Service](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/samples/cellular/nrf_cloud_multi_service/README.html#nrf-cloud-multi-service-provisioning-service)

## Workspace Initialization
Before initializing, start the toolchain environment:
```shell
nrfutil toolchain-manager launch --
```

To initialize the workspace folder (asset-tracker-template) where the firmware project and all nRF Connect SDK modules will be cloned, run the following commands:
```shell
# Initialize asset-tracker-template workspace
west init -m https://github.com/nrfconnect/Asset-Tracker-Template.git --mr main asset-tracker-template

cd asset-tracker-template

# Update nRF Connect SDK modules
west update
```

## Building and running
Complete the following steps for building and running:

1. Navigate to the project folder:
```shell
cd asset-tracker-template
```

2. To build the application, run the following command:
```shell
west build -p -b thingy91x/nrf9151/ns app
```

3. When using the serial bootloader, you can update the application using the following command:
```shell
west thingy91x-dfu
```

4. When using an external debugger, you can program using the following command:
```shell
west flash --erase
```

### Build with memfault
```shell
west build -p -b thingy91x/nrf9151/ns -- -DEXTRA_CONF_FILE="overlay-memfault.conf" -DCONFIG_MEMFAULT_NCS_PROJECT_KEY=\"memfault-project-key\"
```

### Build with memfault and etb traces overlay
```shell
west build -p -b thingy91x/nrf9151/ns -- -DEXTRA_CONF_FILE="overlay-memfault.conf;overlay-etb.conf"
```

### Build with memfault and uploading of modem traces to memfault on coredumps
```shell
west build -p -b thingy91x/nrf9151/ns -- -DEXTRA_CONF_FILE="overlay-memfault.conf;overlay-upload-modem-traces-to-memfault.conf" -DCONFIG_MEMFAULT_NCS_PROJECT_KEY=\"memfault-project-key\"
```
