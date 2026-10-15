# Asset Tracker Template

The Asset Tracker Template is a modular framework for developing IoT applications on nRF91 Series devices. It is built on the [nRF Connect SDK](https://www.nordicsemi.com/Products/Development-software/nRF-Connect-SDK) and [Zephyr RTOS](https://docs.zephyrproject.org/latest/), and provides a modular, event-driven architecture suitable for battery-powered IoT use cases. The framework supports features such as cloud connectivity, location tracking, and sensor data collection.

The system is organized into modules, each responsible for a specific functionality, such as managing network connectivity, handling cloud communication, or collecting environmental data. Modules communicate through [zbus](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/zbus/index.html) channels, ensuring loose coupling and maintainability.

The Asset Tracker Template supports the following hardware:

* [Thingy:91 X](https://www.nordicsemi.com/Products/Development-hardware/Nordic-Thingy-91-X)
* [nRF9151 DK](https://www.nordicsemi.com/Products/Development-hardware/nRF9151-DK)

If you are new to the nRF91 Series and cellular IoT, consider taking the [Cellular IoT Fundamentals](https://academy.nordicsemi.com/courses/cellular-iot-fundamentals/) course from the Nordic Developer Academy.

The following image shows the asset tracking map view in nRF Cloud:

<p align="center">
  <img src="images/att-map.png" alt="Asset tracking map view in nRF Cloud, showing the reported location of a Thingy:91 X" />
  <br>
  <em>The Thingy:91 X running the Asset Tracker Template reports its location to nRF Cloud</em>
</p>

## Get started

To set up your development environment, build the application, flash it to your device, and connect it to [nRF Cloud](https://nrfcloud.com), follow the [Getting started](common/getting_started.md) guide.

## System overview

The following diagram shows the system architecture and how the modules interact with each other.

![Block diagram of the Asset Tracker Template modules and the zbus channels that connect them](images/system_overview.svg)

The template provides the following core modules:

* **[Main module](modules/main.md)**: Implements the business logic and controls the overall application behavior.
* **[Storage module](modules/storage.md)**: Stores data from enabled modules.
* **[Network module](modules/network.md)**: Manages LTE connectivity and tracks network status.
* **[Cloud module](modules/cloud.md)**: Handles communication with nRF Cloud using the Constrained Application Protocol (CoAP).
* **[Location module](modules/location.md)**: Provides location services using Global Navigation Satellite System (GNSS), Wi-Fi, and cellular positioning.
* **[Button module](modules/button.md)**: Reports button press events for user input.
* **[FOTA module](modules/fota_module.md)**: Manages firmware over-the-air updates.

The following modules are specific to the Thingy:91 X:

* **[Environmental module](modules/environmental.md)**: Collects environmental sensor data (temperature, humidity, pressure).
* **[LED module](modules/led.md)**: Controls an RGB LED for visual indication.
* **[Power module](modules/power.md)**: Monitors battery status and provides power management.
* **[UART Power Control module](modules/uart_power_control.md)**: Suspends and resumes the UART when VBUS changes.

## Key features

* **State Machine Framework (SMF)**: Provides predictable behavior through a run-to-completion model.
* **Message-based communication**: Loose coupling through [zbus](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/zbus/index.html) channels.
* **Modular architecture**: Separation of concerns with dedicated threads for blocking operations.
* **Power optimization**: Enables LTE power saving mode (PSM) by default, with configurable power-saving features.
