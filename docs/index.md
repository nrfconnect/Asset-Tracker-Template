# Asset Tracker Template

The Asset Tracker Template is a framework for developing IoT applications on nRF91-based devices. It is built on the [nRF Connect SDK](https://www.nordicsemi.com/Products/Development-software/nRF-Connect-SDK) and [Zephyr RTOS](https://docs.zephyrproject.org/latest/), and provides a modular, event-driven architecture suitable for battery-powered IoT use cases. The framework supports features such as cloud connectivity, location tracking, and sensor data collection.

The system is organized into modules, each responsible for a specific functionality, such as managing network connectivity, handling cloud communication, or collecting environmental data. Modules communicate through [zbus](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/zbus/index.html) channels, ensuring loose coupling and maintainability.

This template is suitable for applications like asset tracking, environmental monitoring, and other IoT use cases requiring modularity, configurability, and efficient power management. It includes a test setup with GitHub Actions for automated testing and continuous integration.

The framework is designed for customization, allowing you to:

* Modify the central business logic in the `main.c` file.
* Enable, disable, and configure modules using Kconfig options.
* Add new modules following the established patterns.
* Modify existing modules to suit specific requirements.
* Contribute to the open-source project by submitting improvements, bug fixes, or new features.

More information on how to customize the template can be found in the [Customization](common/customization.md) document.

**Supported and verified hardware**:

* [Thingy:91 X](https://www.nordicsemi.com/Products/Development-hardware/Nordic-Thingy-91-X)
* [nRF9151 DK](https://www.nordicsemi.com/Products/Development-hardware/nRF9151-DK)

If you are not familiar with the nRF91 Series SiPs and cellular in general, it is recommended to go through the [Nordic Developer Academy Cellular Fundamentals Course](https://academy.nordicsemi.com/courses/cellular-iot-fundamentals) to get a better understanding of the technology and how to customize the template for your needs.


### Key Technical Features

Following are the key features of the template:

1. **State Machine Framework (SMF)**
   * Each module implements its own state machine using Zephyr's [State Machine Framework](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/smf/index.html).
   * The run-to-completion model ensures predictable behavior.

2. **Message-Based Communication (zbus)**
   * Modules communicate through dedicated message channels using [zbus](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/zbus/index.html).

3. **Modular Architecture**
   * Separation of concerns between modules.
   * Each module that performs blocking operations runs in its own thread and uses zbus message subscribers to queue messages.
   * Non-blocking modules use zbus listeners for immediate processing.

4. **Power Optimization**
   * [LTE Power Saving Mode (PSM)](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/protocols/lte/psm.html#power_saving_mode_psm) enabled by default.
   * Configurable power-saving features.
