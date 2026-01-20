# Getting started

To get started with Asset Tracker template, you need to set up the development environment, build the application, and run it on supported hardware.
You can use any of the following tools, depending on your preferred development environment:

* Using Visual Studio Code and the [nRF Connect for VS Code](https://docs.nordicsemi.com/bundle/nrf-connect-vscode/page/index.html)
* Using command line and nRF Util

In nRF Connect for VS Code, the Asset Tracker Template is available as an add-on in the **Create New Application** menu:

![Create New Application menu](../images/create_new_app.png)

Select the **Browse nRF Connect SDK add-on Index** option and search for **Asset Tracker Template**.

![Asset Tracker Template add-on](../images/addon_att.png)

Once you have created the project, you can access various development actions through the **Actions** panel in the nRF Connect for VS Code. These actions provide quick access to common tasks such as building, flashing, and debugging your application:

![Extension actions](../images/actions.png)

For more details on how to use the nRF Connect for VS Code, refer to the [nRF Connect for VS Code documentation](https://docs.nordicsemi.com/bundle/nrf-connect-vscode/page/index.html).

For pre-built binaries, refer to the latest tag and the [release artifacts](release.md) documentation.

## Prerequisites

* **The nRF Util command line tool and the SDK manager command**

    1. Install nRF Util by following the instructions in the [nRF Util documentation](https://docs.nordicsemi.com/bundle/nrfutil/page/guides/installing.html).
    1. Install the SDK manager command by following the instructions in the [sdk-manager command](https://docs.nordicsemi.com/bundle/nrfutil/page/nrfutil-sdk-manager/nrfutil-sdk-manager.html) documentation.

* **nRF Connect SDK toolchain v3.0.0 or later**

    - Follow the instructions in the [sdk-manager command](https://docs.nordicsemi.com/bundle/nrfutil/page/nrfutil-sdk-manager/nrfutil-sdk-manager.html) documentation to install v3.0.0 of the nRF Connect SDK toolchain.

## Supported boards

The Asset Tracker Template is continuously verified in CI on the following boards:

- **[Thingy:91 X](https://www.nordicsemi.com/Products/Development-hardware/Nordic-Thingy-91-X)**

    - Build target `thingy91x/nrf9151/ns`

- **[nRF9151 DK](https://www.nordicsemi.com/Products/Development-hardware/nRF9151-DK)**

    - Build target `nrf9151dk/nrf9151/ns`

## Workspace initialization

Before initializing, start the toolchain environment:

```shell
nrfutil sdk-manager toolchain launch --ncs-version v3.0.0 --shell
```

Alternatively, you can run the command with a specific nRF Connect SDK version. For example, if you are using version 3.0.1, run:

```shell
nrfutil sdk-manager toolchain launch --ncs-version v3.0.0 -- <your command>
```

To run, for instance the `west` command with the specified version of the toolchain. You can create an alias or shell function for this command to avoid typing it in full every time.

In this document, the `nrfutil toolchain-manager launch --shell` variant is used to launch the toolchain environment in the shell.

To initialize the workspace folder (`asset-tracker-template`) where the firmware project and all nRF Connect SDK modules will be cloned, run the following commands:

```shell
# Initialize asset-tracker-template workspace
west init -m https://github.com/nrfconnect/Asset-Tracker-Template.git --mr main asset-tracker-template

cd asset-tracker-template

# Update nRF Connect SDK modules. This may take a while.
west update
```

The template repository is now cloned into the `asset-tracker-template` folder, the west modules are downloaded, and you are ready to build the project.

## Building and running

Complete the following steps for building and running using command line:

1. Navigate to the application folder:

    ```shell
    # Assuming you are in the asset-tracker-template folder
    cd project/app
    ```

1. To build the application, run the following command:

    ```shell
    west build -p -b thingy91x/nrf9151/ns # Pristine build
    ```

1. When using the serial bootloader on Thingy:91 X, you can update the application using the following command:

    ```shell
    west thingy91x-dfu
    ```

1. When using nRF9151 DK or an external debugger on Thingy:91 X, you can program the device using the following command:

    ```shell
    west flash --erase # The --erase option is optional and will erase the entire flash memory before programming
    ```

The application is now built and flashed to the device. You can open a serial terminal to see the logs from the application. The default baud rate is 115200. It is recommended to use the Serial Terminal app, which you can install from [nRF Connect for Desktop](https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-Desktop). You can also use other serial terminal applications like PuTTY, Tera Term, or minicom.

### Building with overlays

You can build the application with different overlays to enable or disable certain features. The following are some examples of how to build the application with different overlays.

Debug build with Memfault:

```shell
west build -p -b thingy91x/nrf9151/ns -- -DEXTRA_CONF_FILE="overlay-memfault.conf;overlay-upload-modem-traces-to-memfault.conf;overlay-etb.conf" -DCONFIG_MEMFAULT_NCS_PROJECT_KEY=\"memfault-project-key\"
```

Build with Memfault, sending modem traces to Memfault and enabling ETB traces:

```shell
west build -p -b thingy91x/nrf9151/ns -- -DEXTRA_CONF_FILE="overlay-memfault.conf;overlay-upload-modem-traces-to-memfault.conf" -DCONFIG_MEMFAULT_NCS_PROJECT_KEY=\"memfault-project-key\"
```

## Provision device to nRF Cloud

To connect to [nRF Cloud](https://nrfcloud.com), the device must be provisioned to your account. You can provision the device using one of the following methods:

* **Quickstart application**: Use the [Quick Start app](https://docs.nordicsemi.com/bundle/nrf-connect-quickstart/page/index.html) in the [nRF Connect for Desktop](https://www.nordicsemi.com/Software-and-Tools/Development-Tools/nRF-Connect-for-desktop) for a streamlined setup process.
* **Manual provisioning**: Follow the detailed steps in the [Provisioning](provisioning.md) documentation.

The provisioning process establishes the necessary credentials and certificates for secure communication between your device and nRF Cloud.

## Testing

To test that everything is working as expected, complete the following steps:

1. In a web browser, navigate to [nRF Cloud](https://nrfcloud.com) and log in to your account. Navigate to the **Device management** menu and select **Devices**. You should now see your device listed in the device overview. Click on the device ID to see the device page.

    ![nRF Cloud device management menu](../images/nrfcloud_devices.png)

1. Connect to the device using the serial terminal and reset the device using either the reset button or the following shell command:

    ```shell
    kernel reboot
    ```

1. In the serial terminal, you will see the device booting up and connecting to the network. The device tries to connect to nRF Cloud using the credentials you provided during provisioning.

    <details>

    <summary>Example log output</summary>

    ```shell
    *** Booting Asset Tracker Template v0.0.0-dev - unknown commit ***
    ***Using nRF Connect SDK v3.1.99-fd20d7a44cf2***
    ***Using Zephyr OS v4.2.99-be5d7776dbb7***
    [00:00:00.522,857] <dbg> main: main: Main has started
    [00:00:00.522,918] <dbg> main: running_entry: running_entry
    [00:00:00.522,949] <dbg> main: passthrough_mode_entry: passthrough_mode_entry
    [00:00:00.522,979] <dbg> main: passthrough_disconnected_entry: passthrough_disconnected_entry
    [00:00:00.523,406] <dbg> cloud: cloud_module_thread: Cloud module task started
    [00:00:00.523,498] <dbg> cloud: state_running_entry: state_running_entry
    [00:00:00.523,559] <inf> nrf_provisioning_coap: Init CoAP client
    [00:00:00.524,078] <dbg> cloud: state_disconnected_entry: state_disconnected_entry
    [00:00:00.524,383] <dbg> main: passthrough_disconnected_entry: passthrough_disconnected_entry
    [00:00:00.524,871] <dbg> environmental: env_module_thread: Environmental module task started
    [00:00:00.524,993] <dbg> fota: fota_module_thread: FOTA module task started
    [00:00:00.525,085] <dbg> fota: state_running_entry: state_running_entry
    [00:00:00.525,939] <dbg> location_module: location_module_thread: Location module task started
    [00:00:00.526,092] <dbg> network: state_running_entry: state_running_entry
    [00:00:00.526,153] <dbg> network: state_running_entry: Bringing network interface up and connecting to the network
    [00:00:00.526,580] <dbg> power: power_module_thread: Power module task started
    [00:00:00.720,947] <inf> nrf_cloud_fota_common: Saved job: , type: 7, validate: 0, bl: 0x0
    [00:00:00.722,351] <dbg> fota: state_waiting_for_poll_request_entry: state_waiting_for_poll_request_entry
    [00:00:00.808,898] <dbg> location_module: state_waiting_for_modem_init_run: Modem initialized, transitioning to running state
    [00:00:00.808,929] <dbg> location_module: state_running_entry: state_running_entry
    [00:00:00.824,981] <dbg> location_module: state_running_entry: Location library initialized
    [00:00:00.825,012] <dbg> location_module: state_location_search_inactive_entry: state_location_search_inactive_entry
    [00:00:00.963,165] <dbg> nrf_provisioning: cert_provision: Certificate already provisioned
    [00:00:00.963,348] <dbg> network: state_running_entry: Network module started
    [00:00:00.963,378] <dbg> network: state_disconnected_entry: state_disconnected_entry
    [00:00:00.963,378] <dbg> network: state_disconnected_searching_entry: state_disconnected_searching_entry
    [00:00:03.496,551] <dbg> network: lte_lc_evt_handler: eDRX parameters received, mode: 7, eDRX: 5.12 s, PTW: 2.56 s
    [00:00:03.497,406] <dbg> network: lte_lc_evt_handler: PDN connection activated
    [00:00:03.498,962] <dbg> cloud: state_connecting_entry: state_connecting_entry
    [00:00:03.498,992] <dbg> cloud: state_connecting_attempt_entry: state_connecting_attempt_entry
    [00:00:03.499,023] <dbg> cloud: state_connecting_provisioned_entry: state_connecting_provisioned_entry
    [00:00:03.499,298] <dbg> network: state_connected_entry: state_connected_entry
    [00:00:03.500,122] <dbg> network: lte_lc_evt_handler: PSM parameters received, TAU: 7200, Active time: 16
    [00:00:03.621,551] <inf> cloud: Connecting to nRF Cloud CoAP with client ID: 5034474b-3731-4772-800e-0d0982488285
    [00:00:05.320,068] <inf> nrf_cloud_coap_transport: Request authorization with JWT
    [00:00:05.567,199] <inf> nrf_cloud_coap_transport: Authorization result_code: 2.01
    [00:00:05.567,321] <inf> nrf_cloud_coap_transport: Authorized
    [00:00:05.567,535] <inf> nrf_cloud_coap_transport: DTLS CID is active
    [00:00:06.096,343] <inf> cloud: nRF Cloud CoAP connection successful
    [00:00:06.096,618] <dbg> cloud: state_connected_entry: state_connected_entry
    [00:00:06.096,649] <inf> cloud: Connected to Cloud
    [00:00:06.096,710] <dbg> cloud: state_connected_ready_entry: state_connected_ready_entry
    [00:00:06.097,045] <dbg> main: passthrough_connected_entry: passthrough_connected_entry
    [00:00:06.097,503] <dbg> main: passthrough_connected_sampling_entry: passthrough_connected_sampling_entry
    [00:00:06.098,510] <dbg> cloud: handle_cloud_channel_message: Poll shadow desired trigger received
    [00:00:06.098,602] <dbg> cloud: cloud_configuration_poll: Configuration: Requesting device shadow desired from cloud
    [00:00:06.100,494] <dbg> location_module: state_location_search_inactive_run: Location search trigger received, starting location request
    [00:00:06.100,524] <dbg> location_module: state_location_search_active_entry: state_location_search_active_entry
    [00:00:06.115,386] <inf> wifi_nrf_bus: SPIM spi@b000: freq = 8 MHz
    [00:00:06.115,417] <inf> wifi_nrf_bus: SPIM spi@b000: latency = 0
    [00:00:06.279,632] <dbg> cbor_helper: decode_shadow_parameters_from_cbor: Command parameter present: type=1, id=1
    [00:00:06.279,754] <dbg> main: config_apply: No configuration parameters to update
    [00:00:06.280,273] <dbg> main: update_shadow_reported_section: Configuration reported: update_interval=600, sample_interval=150, mode=passthrough
    [00:00:06.280,883] <dbg> cloud: cloud_configuration_reported_update: Configuration: Reporting config to cloud
    [00:00:10.948,577] <dbg> location_module: location_event_handler: Cloud location request received from location library
    [00:00:10.948,699] <dbg> location_module: location_cloud_request_data_copy: Copying cloud request data, size of dest: 560
    [00:00:10.948,730] <dbg> location_module: copy_wifi_data: Copied 10 WiFi APs
    [00:00:10.951,416] <dbg> main: passthrough_connected_waiting_entry: passthrough_connected_waiting_entry
    [00:00:10.951,446] <dbg> main: passthrough_connected_waiting_entry: Passthrough mode: next trigger in 596 seconds
    [00:00:10.952,301] <dbg> cloud: handle_cloud_channel_message: Poll shadow delta trigger received
    [00:00:10.952,392] <dbg> cloud: cloud_configuration_poll: Configuration: Requesting device shadow delta from cloud
    [00:00:10.955,963] <dbg> location_module: state_location_search_active_run: Location search done message received, going to inactive state
    [00:00:10.955,993] <dbg> location_module: state_location_search_active_exit: state_location_search_active_exit
    [00:00:10.957,275] <dbg> power: state_running_run: Battery percentage sample request received, getting battery data
    [00:00:10.957,427] <dbg> environmental: state_running_run: Environmental values sample request received, getting data
    [00:00:10.957,519] <dbg> fota: state_polling_for_update_entry: state_polling_for_update_entry
    [00:00:10.957,550] <inf> nrf_cloud_fota_poll: Checking for FOTA job...
    [00:00:10.964,050] <dbg> location_module: state_location_search_inactive_entry: state_location_search_inactive_entry
    [00:00:10.965,209] <dbg> environmental: sample_sensors: Temperature: 23.97 C, Pressure: 98.03 Pa, Humidity: 24.10 %
    [00:00:10.967,102] <dbg> power: sample: State of charge: 97.000000
    [00:00:10.967,132] <dbg> power: sample: The battery is not charging
    [00:00:10.967,163] <dbg> power: sample: Battery voltage: 4.140000 V
    [00:00:10.967,193] <dbg> power: sample: Battery current: -0.000000 A
    [00:00:10.967,193] <dbg> power: sample: Battery temperature: 24.193420 C
    [00:00:11.264,984] <dbg> cloud: cloud_configuration_poll: Shadow delta section not present
    [00:00:11.265,502] <dbg> cloud: handle_storage_data_message: Storage data received, type: 4, size: 568
    [00:00:11.265,594] <dbg> cloud: cloud_location_handle_message: Cloud location request received
    [00:00:11.265,625] <dbg> cloud: handle_cloud_location_request: Handling cloud location request
    [00:00:11.567,504] <inf> nrf_cloud_fota_poll: No pending FOTA job
    [00:00:11.567,535] <dbg> fota: state_polling_for_update_entry: No FOTA job available
    [00:00:11.567,901] <dbg> fota: state_waiting_for_poll_request_entry: state_waiting_for_poll_request_entry
    [00:00:11.846,557] <dbg> cloud: handle_storage_data_message: Storage data received, type: 3, size: 40
    [00:00:12.544,464] <dbg> cloud: cloud_environmental_send: Environmental data sent to cloud: T=24.0°C, P=98.0hPa, H=24.1%
    [00:00:12.544,586] <dbg> cloud: handle_storage_data_message: Storage data received, type: 2, size: 8
    [00:00:12.786,468] <dbg> cloud: send_storage_data_to_cloud: Battery data sent to cloud: 97.0%
    ```

    </details>

1. In the web browser, you will see the device page updating with the latest information from the device, including the location, battery level, and other sensor data.

1. Press **Button 1** on the device to trigger data sampling and sending to nRF Cloud.

    ![nRF Cloud example data](../images/nrf_cloud_example_data.png)

If you experience issues, check the logs in the serial terminal for any error messages. You can find troubleshooting tips in the [Troubleshooting](tooling_troubleshooting.md) section of the documentation.
You can also open a support ticket on [DevZone](https://devzone.nordicsemi.com) for further assistance.

## Next steps

Now that you have the application running, you can:

* **Explore the architecture** - Learn about the modular design in the [Architecture](architecture.md) guide.
* **Configure the application** - Customize behavior using the [Configuration](configuration.md) guide.
* **Perform firmware updates** - Deploy new versions using the [Firmware Updates (FOTA)](fota.md) guide.
* **Customize functionality** - Add or modify features following the [Customization](customization.md) guide.
