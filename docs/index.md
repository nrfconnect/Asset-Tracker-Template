# Asset Tracker Template

The Asset Tracker Template is a modular framework for developing IoT applications on nRF91-based devices. It is built on the [nRF Connect SDK](https://www.nordicsemi.com/Products/Development-software/nRF-Connect-SDK) and [Zephyr RTOS](https://docs.zephyrproject.org/latest/), and provides a modular, event-driven architecture suitable for battery-powered IoT use cases. The framework supports features such as cloud connectivity, location tracking, and sensor data collection.

The system is organized into modules, each responsible for a specific functionality, such as managing network connectivity, handling cloud communication, or collecting environmental data. Modules communicate through [zbus](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/zbus/index.html) channels, ensuring loose coupling and maintainability.

This template is suitable for applications like asset tracking, environmental monitoring, and other IoT use cases requiring modularity, configurability, and efficient power management. It includes a test setup with GitHub Actions for automated testing and continuous integration.

The framework is designed for customization, allowing you to:

- Modify the central business logic in the `main.c` file.
- Enable, disable, and configure modules using Kconfig options.
- Add new modules following the established patterns.
- Modify existing modules to suit specific requirements.
- Contribute to the open-source project by submitting improvements, bug fixes, or new features.

More information on how to customize the template can be found in the [Customization](common/customization.md) document.

The Asset Tracker Template is an add-on and released separately from the [Asset-Tracker-Template](https://github.com/nrfconnect/Asset-Tracker-Template) repository.

**Supported and verified hardware**:

* [Thingy:91 X](https://www.nordicsemi.com/Products/Development-hardware/Nordic-Thingy-91-X)
* [nRF9151 DK](https://www.nordicsemi.com/Products/Development-hardware/nRF9151-DK)

If you are not familiar with the nRF91 Series SiPs and cellular in general, it is recommended to go through the [Nordic Developer Academy Cellular Fundamentals Course](https://academy.nordicsemi.com/courses/cellular-iot-fundamentals) to get a better understanding of the technology and how to customize the template for your needs.

## Quick Start

For detailed setup instructions using the [nRF Connect for VS Code extension](https://docs.nordicsemi.com/bundle/nrf-connect-vscode/page/index.html) and advanced configuration options, see the [Getting Started Guide](docs/common/getting_started.md).

For pre-built binaries, refer to the latest tag and the [release artifacts](common/release.md) documentation.

> [!TIP]
> Download and run the [Quick Start app](https://docs.nordicsemi.com/bundle/nrf-connect-quickstart/page/index.html) in the [nRF Connect for Desktop](https://www.nordicsemi.com/Software-and-Tools/Development-Tools/nRF-Connect-for-desktop) for a guided setup and provisioning process.
>
> You can also refer to [Exercise 1](https://academy.nordicsemi.com/courses/cellular-iot-fundamentals/lessons/lesson-1-cellular-fundamentals/topic/lesson-1-exercise-1/) in [Nordic Developer Academy Cellular Fundamentals Course](https://academy.nordicsemi.com/courses/cellular-iot-fundamentals).

### Prerequisites

* nRF Connect SDK development environment ([setup guide](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/installation.html))

### Build and Run

<details>
<summary>1. <strong>Initialize workspace:</strong></summary>

    ```shell
    # Install nRF Util
    pip install nrfutil

    # or follow install [documentation](https://docs.nordicsemi.com/bundle/nrfutil/page/guides/installing.html)

    # Install toolchain
    nrfutil toolchain-manager install --ncs-version v3.1.0

    # Launch toolchain
    nrfutil toolchain-manager launch --ncs-version v3.1.0 --shell

    # Initialize workspace
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
<ol>
<li>Get the device attestation token over terminal shell:

    ```bash
    at at%attesttoken
    ```

Token is printed automatically on first boot of unprovisioned devices.</li>
<li>Select <strong>Security Services</strong> in the left sidebar.</li>
<li>Select <strong>Claimed Devices</strong>.</li>
<li>Click <strong>Claim Device</strong>.</li>
<li>Copy and paste the attestation token into the <strong>Claim token</strong> text box.</li>
<li>Set rule to nRF Cloud Onboarding and click <strong>Claim Device</strong>.</li>

<details>
<summary><strong>If "nRF Cloud Onboarding" rule is not showing:</strong></summary>

Create a new rule using the following configuration:<br>

<img src="images/claim.png" alt="Claim Device" width="300" />
</details>

<li>Wait for the device to provision credentials and connect to nRF Cloud over CoAP. Once connected, the device must be available under the <strong>Devices</strong> section in the <strong>Device Management</strong> navigation pane on the left.</li>
</ol>

  See <a href="https://docs.nordicsemi.com/bundle/asset-tracker-template-latest/page/common/provisioning.html">provisioning</a> for more details.

</details>

## Key Technical Features

Following are the key features of the template:

* **State Machine Framework (SMF)**

    * Each module implements its own state machine using Zephyr's [State Machine Framework](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/smf/index.html).
    * The run-to-completion model ensures predictable behavior.

* **Message-Based Communication (zbus)**

    * Modules communicate through dedicated message channels using [zbus](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/zbus/index.html).

* **Modular Architecture**

    * Separation of concerns between modules.
    * Each module that performs blocking operations runs in its own thread and uses zbus message subscribers to queue messages.
    * Non-blocking modules use zbus listeners for immediate processing.

* **Power Optimization**

    * [LTE Power Saving Mode (PSM)](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/protocols/lte/psm.html#power_saving_mode_psm) enabled by default.
    * Configurable power-saving features.
