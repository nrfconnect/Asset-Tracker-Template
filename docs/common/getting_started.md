# Getting Started

Before getting started, make sure you have a proper nRF Connect SDK development environment. Follow the official [Getting started guide](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/installation.html)

## Supported boards:
```
thingy91x/nrf9151/ns
nrf9151dk/nrf9151/ns
```

## Workspace Initialization
Before initializing, start the toolchain environment:
```shell
nrfutil toolchain-manager launch --shell
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
cd asset-tracker-template/project/app
```

2. To build the application, run the following command:
```shell
west build -p -b thingy91x/nrf9151/ns
```

3. When using the serial bootloader on thingy91x, you can update the application using the following command:
```shell
west thingy91x-dfu
```

4. When using nrf9151dk or an external debugger on thingy91x, you can program using the following command:
```shell
west flash --erase
```

### Debug build with memfault, modem traces to memfault and etb traces to memfault overlays
```shell
west build -p -b thingy91x/nrf9151/ns -- -DEXTRA_CONF_FILE="overlay-memfault.conf;overlay-upload-modem-traces-to-memfault.conf;overlay-etb.conf" -DCONFIG_MEMFAULT_NCS_PROJECT_KEY=\"memfault-project-key\"
```

## Provision device to nrfcloud
Provisioning steps based on [nRF Cloud Utils](https://github.com/nRFCloud/utils/tree/main).

### Requirements
Do you already have an nRF Cloud account? If not, please visit nrfcloud.com and register. Then, click on the burger on the top-right to get to your user account. Take note of your API key, you will need it soon. Note that if you are part of multiple teams on nRF Cloud, the API key will be different for each one.

Flash device with Asset Tracker Template firmware.

### Steps

Open device shell via serial and set network disconnect mode:
```shell
att_network disconnect
```

Close serial and run following commands

Install nrfcloud-utils package
```shell
pip3 install nrfcloud-utils
```

Create a local certificate authority (CA)
```shell
create_ca_cert
```
Now, you should have three .pem files containing the key pair and the CA certificate of your CA.

Install credentials onto the device
```shell
device_credentials_installer -d --ca *_ca.pem --ca-key *_prv.pem --coap --cmd-type at_shell
```

Upon success, you can find an onboard.csv file with information about your device. We need this file to register the certificate with your account.
```shell
nrf_cloud_onboard --api-key $NRFCLOUD_API_KEY --csv onboard.csv
```

Your device should now be registered to Nrfcloud. You can reset device and wait for cloud connection.
