# Asset Tracker Template

**Oncommit**

[![Target tests](https://github.com/nrfconnect/Asset-Tracker-Template/actions/workflows/build-and-target-test.yml/badge.svg)](https://github.com/nrfconnect/Asset-Tracker-Template/actions/workflows/build-and-target-test.yml)

**Nightly**

[![Target_tests](https://github.com/nrfconnect/Asset-Tracker-Template/actions/workflows/build-and-target-test.yml/badge.svg?event=schedule)](https://github.com/nrfconnect/Asset-Tracker-Template/actions/workflows/build-and-target-test.yml?query=branch%3Amain+event%3Aschedule)
[![Power Consumption Badge](https://img.shields.io/endpoint?url=https://nrfconnect.github.io/Asset-Tracker-Template/power_badge.json)](https://nrfconnect.github.io/Asset-Tracker-Template/power_measurements_plot.html)

[![RAM Usage thingy91x](https://img.shields.io/endpoint?url=https://nrfconnect.github.io/Asset-Tracker-Template/ram_badge.json)](https://nrfconnect.github.io/Asset-Tracker-Template/ram_history_plot.html)
[![Flash Usage thingy91x](https://img.shields.io/endpoint?url=https://nrfconnect.github.io/Asset-Tracker-Template/flash_badge.json)](https://nrfconnect.github.io/Asset-Tracker-Template/flash_history_plot.html)

## Overview

The Asset Tracker Template is a modular application framework designed for nRF91-based IoT devices. It is built on the nRF Connect SDK and leverages Zephyr's features to provide a structured, event-driven system. The framework is intended for battery-powered IoT applications and supports features like cloud connectivity, location tracking, and sensor data collection.

The template is organized into independent modules, each responsible for a specific functionality, such as managing network connectivity, handling cloud communication, or collecting environmental data. These modules communicate through a message-based system, ensuring loose coupling and maintainability.

The template is suitable for use cases such as asset tracking, environmental monitoring, and other IoT applications requiring modularity, configurability, and efficient power management.

It is designed to be easily customizable and extensible, allowing developers to adapt it to their specific needs. The framework includes tests and testing infrastructure, enabling automated testing and continuous integration using GitHub Actions.

**Supported Hardware**:

* [Thingy:91 X](https://www.nordicsemi.com/Products/Development-hardware/Nordic-Thingy-91-X)
* [nRF9151 DK](https://www.nordicsemi.com/Products/Development-hardware/nRF9151-DK)

If you are not familiar with the nRF91 series SiPs and cellular in general, it's recommended to go through the [Nordic Developer Academy Cellular Fundamentals Course](https://academy.nordicsemi.com/courses/cellular-iot-fundamentals) to get a better understanding of the technology and how to customize the template for your needs.

## System Architecture

The template consists of the following modules:

* ***Main Module**: Central coordinator implementing business logic and control flow
* ***Network Module**: Manages LTE connectivity and tracks network status
* ***Cloud Module**: Handles communication with nRF Cloud using CoAP
* ***Location Module**: Provides location services using GNSS, Wi-Fi and cellular positioning
* ***LED Module**: Controls RGB LED for visual feedback on Thingy:91 X
* ***Button Module**: Handles button input for user interaction
* ***FOTA Module**: Manages firmware over-the-air updates
* ***Environmental Module**: Collects environmental sensor data
* ***Power Module**: Monitors battery status and provides power management

The image below illustrates the system architecture and the interaction between the modules.
The zbus channels are indicated with colored arrows showing the direction of communication, with the arrows pointing in to the module that is subscribing to the respective channel.

![System overview](docs/images/system_overview.png)

### Key Technical Features

1. **State Machine Framework (SMF)**
   * Each module implements its own state machine using Zephyr's [State Machine Framework](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/smf/index.html)
   * Run-to-completion model ensures predictable behavior

2. **Message-Based Communication (zbus)**
   * Modules communicate through dedicated [zbus](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/zbus/index.html) channels

3. **Modular Architecture**
   * Separation of concerns between modules
   * Each module that performs blocking operations runs in its own thread and use zbus message subscribers to queue messages
   * Non-blocking modules use zbus listeners for immediate processing

4. **Power Optimization**
   * LTE Power Saving Mode (PSM) enabled by default
   * Configurable power-saving features

### Customization and Extension

The framework is designed to be customizable and intended for developers to adapt it to their specific needs. Key points for customization include:

* The `main.c` file contains the central business logic and is the primary customization point
* Modules can be enabled/disabled via Kconfig options
* New modules can be added following the established patterns
* Existing modules can be modified to suit specific requirements

While designed for asset tracking applications, the template's modular architecture makes it adaptable for various IoT use cases. The template is open-source, and we encourage contributions of improvements, bug fixes, or new features.

## Quick Start

### Prerequisites

* nRF Connect SDK development environment ([Getting started guide](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/installation.html))

### Build and Run

1. Initialize workspace:

```shell
# Launch toolchain
nrfutil toolchain-manager launch --

# Initialize workspace
west init -m https://github.com/nrfconnect/Asset-Tracker-Template.git --mr main asset-tracker-template
cd asset-tracker-template/app
west update
```

2. Build and flash:

```shell
west build -p -b thingy91x/nrf9151/ns
west thingy91x-dfu  # For Thingy91X serial bootloader
# Or if you use an external debugger (ensure that nRF9151 is selected with the switch on the Thingy:91 X)
west flash --erase
```

For detailed setup including nRF Cloud provisioning and advanced build configurations, see [Getting Started](docs/common/getting_started.md).

## Further reading

* [Getting Started](docs/common/getting_started.md)
* [Architecture](docs/common/architecture.md)
* [Configurability](docs/common/configurability.md)
* [Customization](docs/common/customization.md)
* [Location Services](docs/common/location_services.md)
* [Test and CI Setup](docs/common/test_and_ci_setup.md)
* [nRFCloud FOTA](docs/common/nrfcloud_fota.md)
* [Tooling and Troubleshooting](docs/common/tooling_troubleshooting.md)

### Modules

* [Button](docs/modules/button.md)
* [Cloud](docs/modules/cloud.md)
* [Environmental](docs/modules/environmental.md)
* [FOTA](docs/modules/fota.md)
* [LED](docs/modules/led.md)
* [Location](docs/modules/location.md)
* [Main](docs/modules/main.md)
* [Network](docs/modules/network.md)
* [Power](docs/modules/power.md)
