# Lesson 1: Introduction to Asset Tracker Template

## Lesson Overview

In this lesson, you will be introduced to the Asset Tracker Template (ATT), a production-ready framework for developing cellular IoT applications on nRF91 Series devices. You'll learn about its architecture, key features, and design principles, and complete hands-on exercises to build, flash, and provision your first ATT application.

### Learning Objectives

By the end of this lesson, you will be able to:

- Explain the purpose and benefits of the Asset Tracker Template
- Understand the modular architecture and key design principles
- Set up the development environment for ATT
- Build and flash the template to your hardware
- Provision your device with nRF Cloud
- Understand the basic operation and data flow

### Duration

Approximately 90-120 minutes

## 1. Introduction

### 1.1 What is the Asset Tracker Template?

The Asset Tracker Template is a **modular framework** for developing IoT applications on nRF91-based devices. It provides a production-ready starting point for cellular IoT applications that require:

- **Cloud connectivity** - Communication with nRF Cloud using CoAP protocol
- **Location services** - Multiple positioning methods (GNSS, Wi-Fi, cellular)
- **Sensor data collection** - Environmental sensors and battery monitoring
- **Firmware updates** - Over-the-air (FOTA) updates
- **Power efficiency** - Optimized for battery-powered operation

The template is built on the **nRF Connect SDK** and **Zephyr RTOS**, leveraging industry-standard components and best practices.

### 1.2 Why Use the Asset Tracker Template?

Building a cellular IoT application from scratch involves many complex challenges:

- Managing LTE connectivity and power consumption
- Implementing reliable cloud communication
- Handling firmware updates over cellular networks
- Creating maintainable, modular code architecture
- Implementing robust error handling and recovery

The Asset Tracker Template addresses these challenges by providing:

✅ **Production-ready code** - Battle-tested implementation of common IoT features
✅ **Modular architecture** - Easy to understand, modify, and extend
✅ **Best practices** - Follows Nordic and Zephyr design patterns
✅ **Power optimization** - LTE PSM enabled by default
✅ **Cloud integration** - Pre-integrated with nRF Cloud
✅ **Comprehensive testing** - Automated CI/CD pipeline

### 1.3 Key Use Cases

The Asset Tracker Template is suitable for various IoT applications:

- **Asset tracking** - Vehicle, equipment, and package tracking
- **Environmental monitoring** - Temperature, humidity, and air quality sensors
- **Remote monitoring** - Industrial equipment and infrastructure
- **Smart agriculture** - Soil moisture, weather monitoring
- **Fleet management** - Vehicle telemetry and location tracking

## 2. System Architecture

### 2.1 High-Level Overview

The Asset Tracker Template is organized into **independent modules**, each responsible for specific functionality. Modules communicate through message passing using **Zephyr bus (zbus)**, ensuring loose coupling and maintainability.

```
┌─────────────────────────────────────────────────────────────┐
│                      MAIN MODULE                             │
│              (Business Logic Coordinator)                    │
└─────────────┬─────────────────────────────────┬─────────────┘
              │                                 │
              │         ZBUS MESSAGE BUS        │
              │                                 │
    ┌─────────┴─────────┬──────────┬───────────┴────────┐
    │                   │          │                     │
┌───▼────┐  ┌──────▼──────┐  ┌───▼─────┐  ┌──────▼───────┐
│ Network│  │   Cloud     │  │Location │  │Environmental │
│ Module │  │   Module    │  │ Module  │  │   Module     │
└────────┘  └─────────────┘  └─────────┘  └──────────────┘
    │            │               │               │
┌───▼────┐  ┌───▼────┐     ┌───▼────┐     ┌───▼────┐
│Storage │  │  FOTA  │     │  LED   │     │ Power  │
│ Module │  │ Module │     │ Module │     │ Module │
└────────┘  └────────┘     └────────┘     └────────┘
```

### 2.2 Core Modules

The template includes the following core modules:

| Module | Responsibility | Thread |
|--------|---------------|--------|
| **Main** | Central coordinator, business logic | Yes |
| **Network** | LTE connectivity management | Yes |
| **Cloud** | nRF Cloud CoAP communication | Yes |
| **Location** | GNSS, Wi-Fi, cellular positioning | Yes |
| **Storage** | Data collection and buffering | Yes |
| **FOTA** | Firmware over-the-air updates | Yes |
| **Environmental** | Sensor data collection | Yes |
| **Power** | Battery monitoring | No |
| **LED** | RGB LED control | No |
| **Button** | User input handling | No |

### 2.3 Design Principles

The Asset Tracker Template follows these key design principles:

#### Message-Based Communication

Modules communicate exclusively through **zbus channels**, not direct function calls. This ensures:
- **Loose coupling** - Modules are independent
- **Flexibility** - Easy to add, remove, or modify modules
- **Testability** - Modules can be tested in isolation

#### State Machine Framework

Most modules implement state machines using Zephyr's **State Machine Framework (SMF)**:
- **Predictable behavior** - Run-to-completion model
- **Clear state transitions** - Easy to understand and debug
- **Hierarchical organization** - Parent-child state relationships

#### Separation of Concerns

Each module has a **single responsibility**:
- Network module manages connectivity (not business logic)
- Main module coordinates (doesn't handle hardware directly)
- Clear boundaries between modules

#### Thread Management

Modules that perform **blocking operations** have their own thread:
- Network module (modem AT commands)
- Location module (GNSS acquisition)
- Cloud module (network I/O)

Modules with **non-blocking operations** use listeners:
- LED module (immediate LED control)
- Button module (event notification)

## 3. System Operation Flow

### 3.1 Typical Operation Sequence

Here's what happens during a typical data collection cycle:

```
1. SYSTEM STARTUP
   ├─> All modules initialize
   ├─> Network module connects to LTE
   └─> Cloud module connects to nRF Cloud

2. PERIODIC TRIGGER (from Main module timer)
   ├─> Main module publishes LOCATION_REQUEST on LOCATION_CHAN
   └─> Location module receives request

3. LOCATION ACQUISITION
   ├─> Location module starts GNSS
   ├─> Main module sets LED to "acquiring" state
   ├─> Location found after ~30 seconds
   └─> Location module publishes LOCATION_RESPONSE on LOCATION_CHAN

4. SENSOR SAMPLING
   ├─> Main module publishes ENVIRONMENTAL_REQUEST
   ├─> Environmental module samples sensors
   └─> Environmental module publishes ENVIRONMENTAL_RESPONSE

5. DATA TRANSMISSION
   ├─> Storage module collects all data
   ├─> Cloud module sends data to nRF Cloud
   └─> Main module sets LED to "success" state

6. SLEEP
   ├─> Main module enters sleep state
   ├─> LTE enters PSM (Power Saving Mode)
   └─> Wait for next trigger
```

### 3.2 Power Optimization

The template is optimized for battery-powered operation:

- **LTE PSM (Power Saving Mode)** - Enabled by default
- **eDRX** - Configurable extended discontinuous reception
- **Optimized sampling** - Configurable intervals from cloud
- **Sleep management** - System sleep between operations
- **Adaptive backoff** - Intelligent retry mechanisms

Typical power consumption:
- **Active (GNSS + LTE):** ~80-150 mA
- **Idle (PSM):** ~5 µA
- **Average (hourly sampling):** ~5-15 mAh per day

## 4. Development Environment Setup

### 4.1 Prerequisites

Before you begin, ensure you have:

1. **Hardware**
   - Thingy:91 X or nRF9151 DK
   - USB cable
   - (Optional) External debugger for Thingy:91 X

2. **Software**
   - nRF Connect for Desktop installed
   - nRF Connect for VS Code extension installed
   - nRF Command Line Tools
   - nRF Connect SDK v3.1.0 or later

3. **Cloud Account**
   - nRF Cloud account (create at [nrfcloud.com](https://nrfcloud.com))

### 4.2 Installing nRF Connect SDK

If you haven't already installed the nRF Connect SDK:

1. Install **nRF Util**:
   ```bash
   pip install nrfutil
   ```

2. Install the **toolchain**:
   ```bash
   nrfutil toolchain-manager install --ncs-version v3.1.0
   ```

3. Launch the **toolchain environment**:
   ```bash
   nrfutil toolchain-manager launch --ncs-version v3.1.0 --shell
   ```

### 4.3 Cloning the Asset Tracker Template

Initialize a workspace with the Asset Tracker Template:

```bash
# Create and initialize workspace
west init -m https://github.com/nrfconnect/Asset-Tracker-Template.git --mr main asset-tracker-template

# Navigate to the project
cd asset-tracker-template/project/app

# Update dependencies
west update
```

This will create the following structure:
```
asset-tracker-template/
├── project/
│   └── app/               # Main application code
│       ├── src/           # Source code
│       │   ├── main.c
│       │   └── modules/   # Module implementations
│       ├── boards/        # Board-specific configurations
│       └── prj.conf       # Project configuration
├── modules/               # nRF Connect SDK modules
├── zephyr/               # Zephyr RTOS
└── ...
```

## 5. Building and Flashing

### 5.1 Building for Thingy:91 X

Navigate to the app directory and build:

```bash
cd asset-tracker-template/project/app
west build --pristine --board thingy91x/nrf9151/ns
```

The `--pristine` flag ensures a clean build. The build process:
1. Configures the build system (CMake)
2. Compiles application and SDK sources
3. Links all libraries
4. Generates firmware images (app, bootloader)

Build output is located in `build/zephyr/`:
- `merged.hex` - Complete firmware image
- `app_update.bin` - Application for FOTA updates

### 5.2 Building for nRF9151 DK

For the development kit:

```bash
west build --pristine --board nrf9151dk/nrf9151/ns
```

### 5.3 Flashing the Firmware

#### Thingy:91 X with Serial Bootloader

The Thingy:91 X uses a serial bootloader (MCUboot) by default:

```bash
west thingy91x-dfu
```

This command:
1. Puts Thingy:91 X in bootloader mode
2. Transfers firmware over serial
3. Verifies and boots new firmware

#### With External Debugger

If you have a debugger connected:

```bash
west flash --erase
```

The `--erase` flag erases the entire flash before programming, ensuring a clean installation.

### 5.4 Monitoring Serial Output

Connect to the device to see log output:

```bash
# Using nRF Connect for Desktop Terminal
# Or using screen/minicom
screen /dev/ttyACM0 115200
```

You should see output similar to:
```
*** Booting nRF Connect SDK v3.1.0 ***
[00:00:00.123,000] <inf> main: Asset Tracker Template started
[00:00:00.456,000] <inf> network: Connecting to LTE network...
[00:00:05.789,000] <inf> network: Connected to LTE
[00:00:06.012,000] <inf> cloud: Connecting to nRF Cloud...
```

## 6. Provisioning with nRF Cloud

### 6.1 Understanding Device Provisioning

The Asset Tracker Template uses **attestation-based provisioning** with nRF Cloud:

1. **Device Identity** - Each nRF91 device has a unique attestation token
2. **Claiming** - You claim the device in nRF Cloud using this token
3. **Credential Provisioning** - nRF Cloud provisions credentials to the device
4. **Secure Connection** - Device connects using provisioned credentials

This approach provides:
- ✅ **Security** - No pre-shared secrets in firmware
- ✅ **Scalability** - Works for production deployments
- ✅ **Flexibility** - Easy to transfer devices between accounts

### 6.2 Obtaining the Attestation Token

On first boot of an unprovisioned device, the attestation token is automatically printed:

```
[00:00:10.123,000] <inf> cloud: Attestation token:
 bDpCMTg5...very_long_token...9nQiK1JRPgo=
```

Alternatively, you can request it via the shell:

```bash
uart:~$ at at%attesttoken
```

Copy this token - you'll need it for claiming the device.

### 6.3 Claiming the Device in nRF Cloud

1. **Log in to nRF Cloud** at [nrfcloud.com](https://nrfcloud.com)

2. Navigate to **Security Services** → **Claimed Devices**

3. Click **Claim Device**

4. **Paste the attestation token** into the "Claim token" field

5. Select **"nRF Cloud Onboarding"** as the claim rule

   > **Note:** If "nRF Cloud Onboarding" is not available, create a new rule:
   > - Name: `nRF Cloud Onboarding`
   > - Action: `Claim and onboard to nRF Cloud`
   > - Auto-provisioning: `Enabled`

6. Click **Claim Device**

### 6.4 Device Connection

After claiming:

1. The device automatically receives the claim notification
2. Provisions credentials from nRF Cloud
3. Connects to nRF Cloud over CoAP
4. Appears in **Device Management** → **Devices**

You should see log output:
```
[00:00:15.456,000] <inf> cloud: Device claimed successfully
[00:00:16.789,000] <inf> cloud: Provisioning credentials...
[00:00:20.012,000] <inf> cloud: Connected to nRF Cloud
[00:00:20.123,000] <inf> main: System ready
```

### 6.5 Viewing Device Data

Once connected, you can view your device in nRF Cloud:

1. Navigate to **Device Management** → **Devices**
2. Click on your device (identified by IMEI)
3. View real-time data:
   - Location on map
   - Environmental sensor readings
   - Battery status
   - Connection status

## 7. Configuration and Customization

### 7.1 Key Configuration Options

The template behavior can be customized via **Kconfig** options in `prj.conf`:

#### Sampling Intervals

```kconfig
# Environmental sensor sampling
CONFIG_APP_ENVIRONMENTAL_SAMPLE_INTERVAL_SECONDS=300

# Location update interval
CONFIG_APP_LOCATION_TIMEOUT_SECONDS=300
```

#### Network Configuration

```kconfig
# LTE mode (LTE-M or NB-IoT)
CONFIG_LTE_NETWORK_MODE_LTE_M=y
# CONFIG_LTE_NETWORK_MODE_NBIOT=y

# Power Saving Mode
CONFIG_LTE_PSM_REQ=y
```

#### Module Enable/Disable

```kconfig
# Enable/disable modules
CONFIG_APP_ENVIRONMENTAL=y
CONFIG_APP_LOCATION=y
CONFIG_APP_FOTA=y
```

### 7.2 Board-Specific Configuration

Board-specific settings are in `boards/<board_name>.conf`:

```kconfig
# Example: boards/thingy91x_nrf9151_ns.conf

# RGB LED support
CONFIG_LED=y
CONFIG_LED_PWM=y

# Environmental sensors
CONFIG_SENSOR=y
CONFIG_BME680=y
```

### 7.3 Runtime Configuration

Many parameters can be configured **remotely from nRF Cloud**:

1. Navigate to your device in nRF Cloud
2. Go to **Configuration** tab
3. Modify settings:
   - Sampling intervals
   - Location method priorities
   - Update behavior
4. Settings are applied on next connection

## 8. Understanding the Application Code

### 8.1 Main Application Entry Point

The application starts in `src/main.c`:

```c
int main(void)
{
    // Early initialization
    LOG_INF("Asset Tracker Template started");
    
    // System initialization happens via:
    // - SYS_INIT() macros in modules
    // - Thread creation via K_THREAD_DEFINE()
    
    // Main module thread takes over from here
    return 0;
}
```

### 8.2 Main Module Thread

The main module (`src/modules/main/main.c`) implements the business logic:

```c
static void main_task(void)
{
    // Initialize state machine
    smf_set_initial(SMF_CTX(&main_state), &states[STATE_RUNNING]);
    
    // Main event loop
    while (true) {
        // Wait for messages from any subscribed channel
        err = zbus_sub_wait_msg(&main_subscriber,
                               &main_state.chan,
                               main_state.msg_buf,
                               timeout);
        
        // Run state machine with received message
        smf_run_state(SMF_CTX(&main_state));
    }
}
```

### 8.3 Module Structure

Each module follows a similar pattern:

```
modules/<module_name>/
├── <module_name>.h          # Public interface and message definitions
├── <module_name>.c          # Implementation
├── Kconfig.<module_name>    # Configuration options
└── CMakeLists.txt           # Build configuration
```

Example module interface (`network.h`):

```c
/* Zbus channel declaration */
ZBUS_CHAN_DECLARE(NETWORK_CHAN);

/* Message types */
enum network_msg_type {
    NETWORK_CONNECTED,
    NETWORK_DISCONNECTED,
    NETWORK_CONNECTION_TIMEOUT,
};

/* Message structure */
struct network_msg {
    enum network_msg_type type;
    /* Additional fields... */
};
```

## Summary

In this lesson, you learned:

✅ The Asset Tracker Template provides a production-ready framework for cellular IoT applications
✅ The system uses a modular architecture with message-based communication
✅ Modules communicate via zbus and implement state machines using SMF
✅ The development workflow involves building, flashing, and provisioning
✅ Devices connect to nRF Cloud using attestation-based provisioning
✅ Configuration can be done via Kconfig and runtime from nRF Cloud

## Exercise 1: Build, Flash, and Provision Your First Application

See [Exercise 1 instructions](../exercises/lesson1/exercise1/README.md) for hands-on practice building, flashing, and provisioning the Asset Tracker Template.

## Next Steps

In the next lesson, you'll dive deeper into the architecture, specifically learning about **Zbus communication** - how modules communicate through message passing, the difference between subscribers and listeners, and how to add your own custom events.

Continue to [Lesson 2: Architecture - Zbus Communication](lesson2_zbus_communication.md)

