# Asset Tracker Template

**Oncommit**

[![Target tests](https://github.com/nrfconnect/Asset-Tracker-Template/actions/workflows/build-and-target-test.yml/badge.svg)](https://github.com/nrfconnect/Asset-Tracker-Template/actions/workflows/build-and-target-test.yml)

**Nightly**

[![Target_tests](https://github.com/nrfconnect/Asset-Tracker-Template/actions/workflows/build-and-target-test.yml/badge.svg?event=schedule)](https://github.com/nrfconnect/Asset-Tracker-Template/actions/workflows/build-and-target-test.yml?query=branch%3Amain+event%3Aschedule)
[![Power Consumption Badge](https://img.shields.io/endpoint?url=https://nrfconnect.github.io/Asset-Tracker-Template/power_badge.json)](https://nrfconnect.github.io/Asset-Tracker-Template/power_measurements_plot.html)

[![RAM Usage thingy91x](https://img.shields.io/endpoint?url=https://nrfconnect.github.io/Asset-Tracker-Template/ram_badge.json)](https://nrfconnect.github.io/Asset-Tracker-Template/ram_memory_view.html)
[![FLASH Usage thingy91x](https://img.shields.io/endpoint?url=https://nrfconnect.github.io/Asset-Tracker-Template/flash_badge.json)](https://nrfconnect.github.io/Asset-Tracker-Template/flash_memory_view.html)

## Overview

The Asset Tracker Template is a modular framework for developing IoT applications on nRF91-based devices.
It is built on the [nRF Connect SDK](https://www.nordicsemi.com/Products/Development-software/nRF-Connect-SDK) and [Zephyr RTOS](https://docs.zephyrproject.org/latest/), and provides a modular, event-driven architecture suitable for battery-powered IoT use cases.
The framework supports features such as cloud connectivity, location tracking, and sensor data collection.

The system is organized into modules, each responsible for a specific functionality, such as managing network connectivity, handling cloud communication, or collecting environmental data.
Modules communicate through [zbus](https://docs.zephyrproject.org/latest/services/zbus/index.html) channels, ensuring loose coupling and maintainability.

**Supported hardware**: [Thingy:91 X](https://www.nordicsemi.com/Products/Development-hardware/Nordic-Thingy-91-X), [nRF9151 DK](https://www.nordicsemi.com/Products/Development-hardware/nRF9151-DK)

> [!NOTE]
> If you're new to nRF91 series and cellular IoT, consider taking the [Nordic Developer Academy Cellular Fundamentals Course](https://academy.nordicsemi.com/courses/cellular-iot-fundamentals).

## Quick Start

For detailed setup instructions using the [nRF Connect for VS Code extension](https://docs.nordicsemi.com/bundle/nrf-connect-vscode/page/index.html) and advanced configuration options, see the [Getting Started Guide](docs/common/getting_started.md).

For pre-built binaries, refer to the latest tag and release artifacts documentaion; [release artifacts](docs/common/release.md).

> [!TIP]
> Download and run the [Quick Start app](https://docs.nordicsemi.com/bundle/nrf-connect-quickstart/page/index.html) in the [nRF Connect for Desktop](https://www.nordicsemi.com/Software-and-Tools/Development-Tools/nRF-Connect-for-desktop) for a guided setup and provisioning process.
>
> You can also refer to [Exercise 1](https://academy.nordicsemi.com/courses/cellular-iot-fundamentals/lessons/lesson-1-cellular-fundamentals/topic/lesson-1-exercise-1/) in [Nordic Developer Academy Cellular Fundamentals Course](https://academy.nordicsemi.com/courses/cellular-iot-fundamentals) for more details.

### Prerequisites

* nRF Connect SDK development environment ([setup guide](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/installation.html))

### Build and Run

<details>
<summary>1. <strong>Initialize workspace:</strong></summary>

1. Install nRF Util. Follow the <a href="https://docs.nordicsemi.com/bundle/nrfutil/page/guides/installing.html">nRF Util documentation</a> for installation instructions.

2. Install toolchain:

   ```bash
   nrfutil sdk-manager install v3.1.0
	 ```

3. Launch toolchain:

   ```bash
   nrfutil sdk-manager toolchain launch --ncs-version v3.1.0 --terminal
	 ```

4. Initialize workspace:

   ```bash
   west init -m https://github.com/nrfconnect/Asset-Tracker-Template.git --mr main asset-tracker-template
   cd asset-tracker-template/project/app
   west update
   ```
</details>

<details>
<summary>2. <strong>Build and flash:</strong></summary>

**For Thingy:91 X:**
```shell
west build --pristine --board thingy91x/nrf9151/ns
west thingy91x-dfu  # For Thingy:91 X serial bootloader
# Or with external debugger:
west flash --erase
```

**For nRF9151 DK:**
```shell
west build --pristine --board nrf9151dk/nrf9151/ns
west flash --erase
```
</details>

<details>
<summary>3. <strong>Provision device:</strong></summary>

1. Get the device attestation token over terminal shell:

   ```bash
   at at%attesttoken
   ```

   *Note: Token is printed automatically on first boot of unprovisioned devices.*

2. In nRF Cloud: **Security Services** → **Claimed Devices** → **Claim Device**
3. Paste token, set rule to "nRF Cloud Onboarding", click **Claim Device**

    <details>
    <summary><strong>If "nRF Cloud Onboarding" rule is not showing:</strong></summary>

    Create a new rule using the following configuration:

    <img src="docs/images/claim.png" alt="Claim Device" width="300" />
    </details>

4. Wait for the device to provision credentials and connect to nRF Cloud over CoAP. Once connected, the device should be available under **Device Management** → **Devices**.

See [Provisioning to nRF Cloud](docs/common/provisioning.md) for more details.
</details>

## System Architecture

Core modules include:

* **[Main](docs/modules/main.md)**: Central coordinator implementing business logic
* **[Storage](docs/modules/storage.md)**: Data collection and buffering management
* **[Network](docs/modules/network.md)**: LTE connectivity management
* **[Cloud](docs/modules/cloud.md)**: nRF Cloud CoAP communication
* **[Location](docs/modules/location.md)**: GNSS, Wi-Fi, and cellular positioning
* **[LED](docs/modules/led.md)**: RGB LED control for Thingy:91 X
* **[Button](docs/modules/button.md)**: User input handling
* **[FOTA](docs/modules/fota_module.md)**: Firmware over-the-air updates
* **[Environmental](docs/modules/environmental.md)**: Sensor data collection
* **[Power](docs/modules/power.md)**: Battery monitoring and power management

![System overview](docs/images/system_overview.svg)

### Key Features

* **State Machine Framework (SMF)**: Predictable behavior with run-to-completion model
* **Message-Based Communication**: Loose coupling via [zbus](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/zbus/index.html) channels
* **Modular Architecture**: Separation of concerns with dedicated threads for blocking operations
* **Power Optimization**: LTE PSM enabled by default with configurable power-saving features

The architecture is detailed in the [Architecture documentation](docs/common/architecture.md).

## Table of Content

* [Getting Started](docs/common/getting_started.md)
* [Provisioning to nRF Cloud](docs/common/provisioning.md)
* [Architecture](docs/common/architecture.md)
  * [System Overview](docs/common/architecture.md#system-overview)
  * [Zbus](docs/common/architecture.md#zbus)
  * [State Machine Framework](docs/common/architecture.md#state-machine-framework)
* [Configurability](docs/common/configuration.md)
  * [Set sampling interval and logic from cloud](docs/common/configuration.md#set-sampling-interval-and-logic-from-cloud)
  * [Set location method priorities](docs/common/configuration.md#set-location-method-priorities)
  * [Network configuration](docs/common/configuration.md#network-configuration)
  * [LED Status Indicators](docs/common/configuration.md#led-status-indicators)
* [Customization](docs/common/customization.md)
  * [Add a new zbus event](docs/common/customization.md#add-a-new-zbus-event)
  * [Add environmental sensor](docs/common/customization.md#add-environmental-sensor)
  * [Add your own module](docs/common/customization.md#add-your-own-module)
  * [Enable support for MQTT](docs/common/customization.md#enable-support-for-mqtt)
* [Location Services](docs/common/location_services.md)
* [Achieving Low Power](docs/common/low_power.md)
* [Test and CI Setup](docs/common/test_and_ci_setup.md)
* [Firmware Updates (FOTA)](docs/common/fota.md)
* [Tooling and Troubleshooting](docs/common/tooling_troubleshooting.md)
  * [Shell Commands](docs/common/tooling_troubleshooting.md#shell-commands)
  * [Debugging Tools](docs/common/tooling_troubleshooting.md#debugging-tools)
  * [Memfault Remote Debugging](docs/common/tooling_troubleshooting.md#memfault-remote-debugging)
  * [Modem Tracing](docs/common/tooling_troubleshooting.md#modem-tracing)
  * [Common Issues and Solutions](docs/common/tooling_troubleshooting.md#common-issues-and-solutions)
* [Known Issues](docs/common/known_issues.md)
* [Release artifacts](docs/common/release.md)

### Module Documentation

* [Button](docs/modules/button.md)
* [Cloud](docs/modules/cloud.md)
* [Storage](docs/modules/storage.md)
* [Environmental](docs/modules/environmental.md)
* [FOTA](docs/modules/fota_module.md)
* [LED](docs/modules/led.md)
* [Location](docs/modules/location.md)
* [Main](docs/modules/main.md)
* [Network](docs/modules/network.md)
* [Power](docs/modules/power.md)
