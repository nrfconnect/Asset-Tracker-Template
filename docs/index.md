# Asset Tracker Template

The Asset Tracker Template is a modular framework for developing IoT applications on nRF91-based devices. It is built on the [nRF Connect SDK](https://www.nordicsemi.com/Products/Development-software/nRF-Connect-SDK) and [Zephyr RTOS](https://docs.zephyrproject.org/latest/), and provides a modular, event-driven architecture suitable for battery-powered IoT use cases. The framework supports features such as cloud connectivity, location tracking, and sensor data collection.

The system is organized into modules, each responsible for a specific functionality, such as managing network connectivity, handling cloud communication, or collecting environmental data. Modules communicate through [zbus](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/zbus/index.html) channels, ensuring loose coupling and maintainability.

**Supported hardware**:

* [Thingy:91 X](https://www.nordicsemi.com/Products/Development-hardware/Nordic-Thingy-91-X)
* [nRF9151 DK](https://www.nordicsemi.com/Products/Development-hardware/nRF9151-DK)

If you are new to nRF91 series and cellular IoT, consider taking the [Nordic Developer Academy Cellular Fundamentals Course](https://academy.nordicsemi.com/courses/cellular-iot-fundamentals).

<p align="center">
  <img src="images/att-map.png" alt="nRF Cloud - Asset tracking map view" width="800" />
  <br>
  <em>Thingy:91 X reporting its location to nRF Cloud running the Asset Tracker Template</em>
</p>

## Quick Start

The fastest way to get started is to download and run the [Quick Start app](https://docs.nordicsemi.com/bundle/nrf-connect-quickstart/page/index.html) in [nRF Connect for Desktop](https://www.nordicsemi.com/Software-and-Tools/Development-Tools/nRF-Connect-for-desktop). It provides a guided setup and provisioning process that gets your device connected to [nRF Cloud](https://nrfcloud.com) in minutes.

Alternatively, you can download pre-built firmware binaries from the [latest release](https://github.com/nrfconnect/Asset-Tracker-Template/releases) and flash them directly to your device. See the [release artifacts](common/release.md) documentation for details.

## Get started

To set up your development environment, build the application, flash it to your device, and connect it to [nRF Cloud](https://nrfcloud.com), follow the [Getting Started](common/getting_started.md) guide.

## System Overview

![System overview](images/system_overview.svg)

Core modules include:

* **[Main](modules/main.md)**: Central coordinator implementing business logic
* **[Storage](modules/storage.md)**: Data collection and buffering management
* **[Network](modules/network.md)**: LTE connectivity management
* **[Cloud](modules/cloud.md)**: nRF Cloud CoAP communication
* **[Location](modules/location.md)**: GNSS, Wi-Fi, and cellular positioning
* **[LED](modules/led.md)**: RGB LED control for Thingy:91 X
* **[Button](modules/button.md)**: User input handling
* **[FOTA](modules/fota_module.md)**: Firmware over-the-air updates
* **[Environmental](modules/environmental.md)**: Sensor data collection
* **[Power](modules/power.md)**: Battery monitoring and power management

### Key Features

* **State Machine Framework (SMF)**: Predictable behavior with run-to-completion model
* **Message-Based Communication**: Loose coupling via [zbus](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/zbus/index.html) channels
* **Modular Architecture**: Separation of concerns with dedicated threads for blocking operations
* **Power Optimization**: LTE PSM enabled by default with configurable power-saving features
