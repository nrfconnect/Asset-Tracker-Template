# Getting Started

Before getting started, make sure you have a proper nRF Connect SDK development environment. Follow the official [Getting started guide](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/installation.html)


## Initialization
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

## Supported boards:
```shell
thingy91x/nrf9151/ns
nrf9151dk/nrf9151/ns
```

## Building and running
Complete the following steps for building and running:

1. Navigate to the project folder:
```shell
cd asset-tracker-template
```

2. To build the application, run the following command:
```shell
west build -b thingy91x/nrf9151/ns app
```

3. When using an external debugger, you can program using the following command:
```shell
west thingy91x-dfu
```

4. When using an external debugger, you can program using the following command:
```shell
west flash --erase
```

### Build with memfault
```shell
west build -b thingy91x/nrf9151/ns -- -DEXTRA_CONF_FILE="overlay-memfault.conf"
```

### Build with memfault and etb traces overlay
```shell
west build -b thingy91x/nrf9151/ns -- -DEXTRA_CONF_FILE="overlay-memfault-debug.conf;overlay-etb.conf"
```
