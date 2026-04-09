# Asset Tracker Template

The Asset Tracker Template is a modular framework for developing IoT applications on nRF91-based devices. It is built on the [nRF Connect SDK](https://www.nordicsemi.com/Products/Development-software/nRF-Connect-SDK) and [Zephyr RTOS](https://docs.zephyrproject.org/latest/), and provides a modular, event-driven architecture suitable for battery-powered IoT use cases. The framework supports features such as cloud connectivity, location tracking, and sensor data collection.

The system is organized into modules, each responsible for a specific functionality, such as managing network connectivity, handling cloud communication, or collecting environmental data. Modules communicate through [zbus](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/zbus/index.html) channels, ensuring loose coupling and maintainability.

**Supported hardware**:

* [Thingy:91 X](https://www.nordicsemi.com/Products/Development-hardware/Nordic-Thingy-91-X)
* [nRF9151 DK](https://www.nordicsemi.com/Products/Development-hardware/nRF9151-DK)

If you are new to nRF91 series and cellular IoT, consider taking the [Nordic Developer Academy Cellular Fundamentals Course](https://academy.nordicsemi.com/courses/cellular-iot-fundamentals).

## Quick Start

The fastest way to get started is to download and run the [Quick Start app](https://docs.nordicsemi.com/bundle/nrf-connect-quickstart/page/index.html) in [nRF Connect for Desktop](https://www.nordicsemi.com/Software-and-Tools/Development-Tools/nRF-Connect-for-desktop). It provides a guided setup and provisioning process that gets your device connected to [nRF Cloud](https://nrfcloud.com) in minutes.

Alternatively, you can download pre-built firmware binaries from the latest release and flash them directly to your device. See the [release artifacts](common/release.md) documentation for details.

## Setting up the development environment

There are two options for setting up and building the project.

### Option 1: nRF Connect for VS Code (Recommended)

Use the [nRF Connect for VS Code](https://docs.nordicsemi.com/bundle/nrf-connect-vscode/page/index.html) for an integrated development experience:

1. To install the nRF Connect SDK and its toolchain using nRF Connect for VS Code, follow [extension documentation](https://docs.nordicsemi.com/bundle/nrf-connect-vscode/page/get_started/quick_setup.html) or [Installing nRF Connect SDK and VS Code exercise](https://academy.nordicsemi.com/courses/nrf-connect-sdk-fundamentals/lessons/lesson-1-nrf-connect-sdk-introduction/topic/exercise-1-1/) on Nordic Developer Academy.
2. Open VS Code and go to the **nRF Connect** extension.
3. Select **Create New Application**.
4. Select **Browse nRF Connect SDK add-on Index**.
4. Search for **Asset Tracker Template** and create the project.
5. Use the **Actions** panel in the extension to build and flash the application.

### Option 2: Command line

<details>
<summary><strong>1. Initialize workspace</strong></summary>
<ol>
<li>Install nRF Util. Follow <a href="https://docs.nordicsemi.com/bundle/nrfutil/page/guides/installing.html">documentation</a> for installation instructions.</li>
<li>Install sdk-manager:
<pre><code class="language-bash">nrfutil install sdk-manager</code></pre>
</li>
<li>Install toolchain:
<pre><code class="language-bash">nrfutil sdk-manager install v3.1.0</code></pre>
</li>
<li>Launch toolchain:
<pre><code class="language-bash">nrfutil sdk-manager toolchain launch --ncs-version v3.1.0 --terminal</code></pre>
This launches a new terminal window with the specified toolchain activated. Use this terminal in the coming steps.
</li>
<li>Initialize workspace
<pre><code class="language-bash">west init -m https://github.com/nrfconnect/Asset-Tracker-Template.git --mr main asset-tracker-template
cd asset-tracker-template/project/app
west update</code></pre>
</li>
</ol>
</details>

<details>
<summary><strong>2. Build and flash</strong></summary>

<strong>For Thingy:91 X:</strong>

```shell
west build --pristine --board thingy91x/nrf9151/ns
west thingy91x-dfu  # For Thingy:91 X serial bootloader
# Or with external debugger:
west flash --erase
```

<strong>For nRF9151 DK:</strong>

```shell
west build --pristine --board nrf9151dk/nrf9151/ns
west flash --erase
```
</details>

For more details, see the [Getting Started Guide](common/getting_started.md).

### Provision device to nRF Cloud

After building and flashing (using either option above), you need to provision the device to connect to [nRF Cloud](https://nrfcloud.com).

<details>
<summary><strong>Provisioning steps</strong></summary>
<ol>
<li>Get the device attestation token. Open a serial terminal connected to the device (115200 baud) and run the following AT command in the device shell:
<pre><code class="language-bash">at at%attesttoken</code></pre>
The token is also printed automatically to the serial log on first boot of unprovisioned devices.</li>
<li>Log in to the <a href="https://nrfcloud.com/#/">nRF Cloud</a> portal.</li>
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

<li>After claiming, wait for the device to provision credentials and connect to nRF Cloud over CoAP. Once connected, the device will be available under the <strong>Devices</strong> section in the <strong>Device Management</strong> navigation pane on the left.</li>
</ol>

<blockquote>
<strong>Note:</strong> The device polls the provisioning service at its own interval. This means it may take a few minutes for the device to pick up the claim and complete provisioning. If you want a quicker response, press and hold <strong>Button 1</strong> on the device or reset the device to trigger an immediate provisioning poll.
</blockquote>

See <a href="https://docs.nordicsemi.com/bundle/asset-tracker-template-latest/page/common/provisioning.html">provisioning</a> for more details.

</details>

## Key Features

* **State Machine Framework (SMF)**: Predictable behavior with run-to-completion model
* **Message-Based Communication**: Loose coupling via [zbus](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/zbus/index.html) channels
* **Modular Architecture**: Separation of concerns with dedicated threads for blocking operations
* **Power Optimization**: LTE PSM enabled by default with configurable power-saving features

The architecture is detailed in the [Architecture documentation](common/architecture.md).
