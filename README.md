# Asset Tracker Template

#### Oncommit:
[![Target tests](https://github.com/nrfconnect/Asset-Tracker-Template/actions/workflows/build-and-target-test.yml/badge.svg)](https://github.com/nrfconnect/Asset-Tracker-Template/actions/workflows/build-and-target-test.yml)

#### Nightly:
[![Target_tests](https://github.com/nrfconnect/Asset-Tracker-Template/actions/workflows/build-and-target-test.yml/badge.svg?event=schedule)](https://github.com/nrfconnect/Asset-Tracker-Template/actions/workflows/build-and-target-test.yml?query=branch%3Amain+event%3Aschedule)
[![Power Consumption Badge](https://img.shields.io/endpoint?url=https://nrfconnect.github.io/Asset-Tracker-Template/power_badge.json)](https://nrfconnect.github.io/Asset-Tracker-Template/power_measurements_plot.html)


[![RAM Usage thingy91x](https://img.shields.io/endpoint?url=https://nrfconnect.github.io/Asset-Tracker-Template/ram_badge.json)](https://github.com/nrfconnect/Asset-Tracker-Template/actions/workflows/build-and-target-test.yml)
[![Flash Usage thingy91x](https://img.shields.io/endpoint?url=https://nrfconnect.github.io/Asset-Tracker-Template/flash_badge.json)](https://github.com/nrfconnect/Asset-Tracker-Template/actions/workflows/build-and-target-test.yml)

## Overview

The Asset Tracker Template implements a modular application framework for nRF91-based IoT devices.
The application is built on nRF Connect SDK and uses a combination of state machines and message-based inter-module communication.
It is intended to be a framework for developing asset tracking applications, but can be customized for other use cases as well.
We have not targeted a specific use-case with this template, but rather a set of features that are common in asset tracking applications, and left it up to the user to decide how to use, modify, or extend the template to fit their needs.
The ``main.c`` file contains the main module of the application, where the business logic and control over the other modules is implemented, and is a good starting point for understanding how the application works.
This is also the natural place to start when customizing the application for a specific use-case.

The template is open-source for a reason, and we encourage users to contribute back to the project with improvements, bug fixes, or new features.

## Key concepts

* **Modular design**: The application is divided into modules, each responsible for a specific feature or functionality.
* **State machines**: Each module, where applicable, has its own state machine, which defines the behavior of the module. The state machines are implemented using Zephyr's [State Machine Framework](https://docs.nordicsemi.com/bundle/ncs-3.0.0-preview1/page/zephyr/services/smf/index.html).
* **Message-based communication**: Modules communicate with each other by sending messages using [zbus](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/zbus/index.html), a message bus library for Zephyr.
* **Configuration options**: The application can be configured using Kconfig options to enable or disable features, set parameters, and more.
* **Designed for low-power operation**: The application is designed to be power-efficient, with features like LTE Power Saving Mode (PSM) enabled by default.


## Table of Contents

### Common
- [Business Logic](docs/common/business_logic.md)
- [Configurability](docs/common/configurability.md)
- [Customization](docs/common/customization.md)
- [Location Services](docs/common/location_services.md)
- [Test and CI Setup](docs/common/test_and_ci_setup.md)
- [Tooling](docs/common/tooling.md)
- [Optimizations](docs/common/optimizations.md)
- [Advanced](docs/common/advanced.md)

### Modules
- [Main](docs/modules/main.md)
- [Location]((docs/modules/location.md))
- [LED]((docs/modules/led.md))
- [FOTA](docs/modules/fota.md)
- [Button](docs/modules/button.md)
- [Cloud](docs/modules/cloud.md)
- [Environmental](docs/modules/environmental.md)
- [Network](docs/modules/network.md)
- [Power](docs/modules/power.md)
