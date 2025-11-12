# Lesson 1: Introduction to Asset Tracker Template

## Learning Objectives

By the end of this lesson, you will be able to:

- Understand the purpose and key features of the Asset Tracker Template
- Set up your development environment for ATT development
- Build and flash the template to Thingy:91 X or nRF9151 DK
- Provision a device to nRF Cloud and verify cloud connectivity
- Navigate the ATT project structure and identify key components

## Prerequisites

- Completed the [Cellular IoT Fundamentals Course](https://academy.nordicsemi.com/courses/cellular-iot-fundamentals/)
- nRF Connect SDK v3.0.0 or later installed
- Hardware: Thingy:91 X or nRF9151 DK
- Active nRF Cloud account ([sign up here](https://nrfcloud.com))

**Estimated Duration:** 1.5 hours

---

## Introduction

Building production-quality cellular IoT applications requires more than just connecting a sensor to the cloud. You need:

- **Robust architecture** that can scale and adapt to changing requirements
- **Power efficiency** to maximize battery life in field deployments
- **Reliable communication** that handles network disruptions gracefully
- **Maintainable code** that multiple developers can work on
- **Field update capability** to fix bugs and add features remotely

The **Asset Tracker Template** provides all of this out of the box. It's not just sample code—it's a production-ready framework that embodies years of best practices from Nordic Semiconductor and the wider embedded community.

### What is the Asset Tracker Template?

The Asset Tracker Template is a reference application and framework for building cellular IoT applications on nRF91 Series devices. It demonstrates:

- **Modular architecture** - Each functionality (networking, cloud, sensors, location) is isolated in its own module
- **Event-driven design** - Modules communicate through message passing, not direct function calls
- **State machines** - Complex behavior is managed with the State Machine Framework (SMF)
- **Power optimization** - Configured for low power consumption with LTE PSM support
- **Cloud integration** - Ready-to-use nRF Cloud integration via CoAP
- **Field updates** - Full FOTA (Firmware Over-The-Air) support

Think of ATT as a professional starting point. Instead of building everything from scratch, you start with a solid foundation and customize it for your specific needs.

### Key Features at a Glance

| Feature | Description |
|---------|-------------|
| **Modules** | Main, Network, Cloud, Storage, Location, Environmental, Button, LED, Power, FOTA |
| **Communication** | Zbus message passing for loose coupling |
| **State Management** | SMF (State Machine Framework) for predictable behavior |
| **Cloud Service** | nRF Cloud via CoAP (with MQTT example available) |
| **Location** | GNSS, Wi-Fi, and cellular positioning |
| **Power Modes** | LTE PSM and eDRX support for battery optimization |
| **Updates** | Full application and modem FOTA support |
| **Hardware** | Thingy:91 X, nRF9151 DK, nRF9160 DK, nRF9161 DK |

### Why Use a Template?

You might wonder: "Why not just start from scratch?" Here's why ATT is valuable:

1. **Learn from experts** - The template embodies Nordic's recommended patterns and practices
2. **Save time** - Months of architecture work are already done
3. **Avoid pitfalls** - Common issues (power management, state handling, error recovery) are solved
4. **Production-ready** - Includes features often forgotten in prototypes (FOTA, error handling, logging)
5. **Well-tested** - Continuous integration ensures reliability across SDK versions

**Important:** ATT is designed to be customized. You're not locked into its choices—you'll learn how to modify and extend it to fit your requirements.

---

## Architecture Overview

Before we dive into building and running the template, let's get a high-level understanding of its architecture.

### The Big Picture

```
┌─────────────────────────────────────────────────────────────┐
│                        Main Module                           │
│            (Business Logic & Coordination)                   │
└───────────────────┬─────────────────────────────────────────┘
                    │
        ┌───────────┴───────────┐
        │      Zbus Channels     │  ← Message Bus
        └───────────┬───────────┘
                    │
    ┌───────────────┼───────────────┬──────────────┬──────────┐
    │               │               │              │          │
┌───▼───┐      ┌───▼────┐     ┌───▼────┐    ┌───▼───┐  ┌───▼────┐
│Network│      │ Cloud  │     │Location│    │Storage│  │  LED   │
└───────┘      └────────┘     └────────┘    └───────┘  └────────┘
    │               │              │             │          │
┌───▼───┐      ┌───▼────┐     ┌───▼────┐    ┌───▼───┐  ┌───▼────┐
│ Modem │      │nRF Cloud│     │  GNSS  │    │ Flash │  │RGB LED │
└───────┘      └────────┘     └────────┘    └───────┘  └────────┘
```

### Key Architectural Concepts

**1. Modular Design**

Each module has a single, well-defined responsibility:
- **Main module**: Implements business logic, coordinates other modules
- **Network module**: Manages LTE connectivity
- **Cloud module**: Handles communication with nRF Cloud
- **Storage module**: Buffers data when cloud is unavailable
- **Location module**: Provides position via GNSS/Wi-Fi/cellular
- **Environmental module**: Reads sensor data (temperature, humidity, pressure)
- **Button module**: Handles user input
- **LED module**: Provides visual feedback
- **Power module**: Monitors battery and manages power
- **FOTA module**: Handles firmware updates

**2. Message-Based Communication (Zbus)**

Modules don't call each other's functions directly. Instead, they send messages over Zbus channels:

```c
// Main module requests location
struct location_msg msg = {
    .type = LOCATION_SEARCH_REQUEST
};
zbus_chan_pub(&LOCATION_CHAN, &msg, K_SECONDS(1));
```

This loose coupling means:
- Modules can be developed and tested independently
- Changes to one module rarely break others
- New modules can be added without modifying existing code

**3. State Machines (SMF)**

Complex modules use state machines to manage their behavior:

```
Network Module States:
RUNNING → DISCONNECTED → SEARCHING → CONNECTED
```

Each state has clearly defined:
- **Entry actions** - What to do when entering the state
- **Run actions** - How to handle messages while in the state
- **Exit actions** - Cleanup when leaving the state

This provides predictable, testable behavior.

**4. Threading Model**

- Modules with blocking operations (Network, Cloud, Location, etc.) run in their own threads
- Non-blocking modules (LED) execute in the caller's context as listeners
- Each thread has watchdog monitoring for robustness

### Data Flow Example

Let's trace what happens when you press a button:

1. **Button module** detects press, publishes `BUTTON_PRESSED` message
2. **Main module** receives message, transitions to "sample data" state
3. **Main** publishes `LOCATION_SEARCH_REQUEST` message
4. **Location module** receives request, starts GNSS/Wi-Fi scan
5. **Location** publishes `LOCATION_SEARCH_RESPONSE` with coordinates
6. **Main** publishes `ENVIRONMENTAL_SAMPLE_REQUEST`
7. **Environmental module** reads sensors, publishes `ENVIRONMENTAL_SAMPLE_RESPONSE`
8. **Main** publishes `STORAGE_NEW_DATA` for each data type
9. **Storage module** forwards data to Cloud (or buffers if offline)
10. **Cloud module** encodes and sends data to nRF Cloud via CoAP

Throughout this flow, the **LED module** provides visual feedback based on messages it observes.

Notice: Each module only knows about messages, not other modules. This is the power of event-driven architecture!

---

## Project Structure

Let's explore the directory layout so you know where to find things:

```
asset-tracker-template/
├── app/                          # Main application
│   ├── src/
│   │   ├── main.c               # Application entry point
│   │   ├── modules/             # All modules live here
│   │   │   ├── main/            # Main module (business logic)
│   │   │   ├── network/         # Network connectivity
│   │   │   ├── cloud/           # Cloud communication
│   │   │   ├── storage/         # Data buffering
│   │   │   ├── location/        # Positioning services
│   │   │   ├── environmental/   # Sensor data
│   │   │   ├── button/          # Button input
│   │   │   ├── led/             # LED control
│   │   │   ├── power/           # Battery monitoring
│   │   │   └── fota/            # Firmware updates
│   │   └── cbor/                # Data encoding
│   ├── boards/                  # Board-specific configs
│   │   ├── thingy91x_nrf9151_ns.conf
│   │   ├── thingy91x_nrf9151_ns.overlay
│   │   ├── nrf9151dk_nrf9151_ns.conf
│   │   └── nrf9151dk_nrf9151_ns.overlay
│   ├── prj.conf                 # Main configuration file
│   ├── CMakeLists.txt           # Build system
│   └── VERSION                  # Version tracking
├── docs/                        # Documentation
│   ├── common/
│   │   ├── architecture.md      # Architecture deep-dive
│   │   ├── customization.md     # How to extend ATT
│   │   ├── getting_started.md   # Setup guide
│   │   └── ...
│   └── modules/                 # Per-module documentation
├── examples/                    # Example extensions (MQTT cloud)
├── tests/                       # Unit and integration tests
└── README.md                    # Quick start guide
```

### Key Files to Know

- **`app/src/main.c`** - Entry point, but most logic is in the Main module
- **`app/src/modules/main/main.c`** - Core business logic and state machine
- **`app/prj.conf`** - Kconfig options for the entire application
- **`app/boards/[board].conf`** - Board-specific Kconfig overrides
- **`app/boards/[board].overlay`** - Device tree overlays for hardware
- **`docs/common/architecture.md`** - Detailed architecture documentation
- **`docs/common/customization.md`** - Guide for extending ATT

---

## Exercise 1: Build and Deploy the Template

Now let's get hands-on! In this exercise, you'll build the Asset Tracker Template and flash it to your device.

**Objective:** Successfully build and deploy the ATT to your hardware, then verify it boots correctly.

**Duration:** 30 minutes

### Task Description

You will set up a fresh workspace, build the template for your specific board, and flash it. Then you'll connect a serial terminal to verify the application is running.

### Step-by-Step Instructions

#### Step 1: Initialize Your Workspace

Open a terminal and initialize the ATT workspace:

```bash
# Launch the toolchain for nRF Connect SDK v3.1.0
nrfutil toolchain-manager launch --ncs-version v3.1.0 --shell

# Initialize workspace
west init -m https://github.com/nrfconnect/Asset-Tracker-Template.git --mr main asset-tracker-template

# Navigate into the workspace
cd asset-tracker-template

# Update all dependencies (this takes a few minutes)
west update
```

**What's happening:**
- `west init` creates a West workspace and clones the ATT repository
- `west update` fetches all nRF Connect SDK modules (Zephyr, nRF libraries, etc.)

#### Step 2: Navigate to the Application Directory

```bash
cd project/app
```

This is where you'll build from. The `app/` directory contains the main application code.

#### Step 3: Build for Your Board

**For Thingy:91 X:**
```bash
west build -p -b thingy91x/nrf9151/ns
```

**For nRF9151 DK:**
```bash
west build -p -b nrf9151dk/nrf9151/ns
```

**Build flags explained:**
- `-p` : Pristine build (clean rebuild)
- `-b` : Board target
- `ns` : Non-secure (required for TF-M trusted firmware)

Build time: 3-5 minutes on first build (incremental builds are much faster).

**Expected output:**
```
...
Memory region         Used Size  Region Size  %age Used
           FLASH:      387376 B       984 KB     38.45%
             RAM:      135600 B       440 KB     30.10%
        IDT_LIST:          0 GB         2 KB      0.00%
[217/217] Linking C executable zephyr/zephyr.elf
```

#### Step 4: Flash to Your Device

**For Thingy:91 X (using serial bootloader):**

1. Put Thingy:91 X in bootloader mode:
   - Turn off the device
   - Hold the multi-function button while turning it on
   - The LED will show yellow to indicate bootloader mode

2. Flash via USB:
```bash
west thingy91x-dfu
```

**For Thingy:91 X (with external debugger):**
```bash
west flash --erase
```

**For nRF9151 DK:**
```bash
west flash --erase
```

**What's happening:**
- The application, bootloader (MCUboot), and TF-M (Trusted Firmware) are all flashed
- `--erase` ensures a clean flash without remnants of previous firmware

#### Step 5: Connect Serial Terminal

The device communicates via UART. Connect a serial terminal to see logs:

**Recommended: nRF Connect Serial Terminal**
1. Install [nRF Connect for Desktop](https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-Desktop)
2. Install the Serial Terminal app
3. Connect to your device (115200 baud, 8N1)

**Alternative: Using screen (Linux/Mac)**
```bash
# Find the device
ls /dev/ttyACM* 

# Connect (replace ttyACM0 with your device)
screen /dev/ttyACM0 115200
```

**Alternative: Using PuTTY (Windows)**
- Select Serial connection
- Set COM port (check Device Manager)
- Set baud rate to 115200

#### Step 6: Verify Boot

After connecting, reset the device (press reset button or power cycle). You should see:

```
*** Booting nRF Connect SDK v3.0.0-3bfc46578e42 ***
*** Using Zephyr OS v4.0.99-a0e545cb437a ***
...
[00:00:00.520,202] <dbg> main: main: Main has started
[00:00:00.520,263] <dbg> main: running_entry: running_entry
[00:00:00.520,294] <dbg> main: idle_entry: idle_entry
...
[00:00:00.520,812] <dbg> cloud: cloud_thread: cloud module task started
...
[00:00:00.523,345] <dbg> network: state_running_entry: state_running_entry
[00:00:00.523,376] <inf> network: Bringing network interface up and connecting to the network
```

**Key things to observe:**
- ✅ Boot messages from MCUboot and TF-M
- ✅ Each module starting up (main, cloud, network, location, etc.)
- ✅ Network module attempting to connect to LTE
- ✅ No error messages or crashes

**Note:** The device won't fully connect to nRF Cloud yet—you need to provision it first (next exercise).

### Expected Outcome

At this point you should have:
- Successfully built the ATT for your board
- Flashed it to your device
- Observed the boot sequence in the serial terminal
- Seen modules starting up without errors

If the device boots and you see module initialization messages, congratulations! You've successfully deployed the Asset Tracker Template.

### Troubleshooting

| Issue | Solution |
|-------|----------|
| **Build fails with "west not found"** | Ensure you're in the toolchain shell: `nrfutil toolchain-manager launch --ncs-version v3.1.0 --shell` |
| **"No such file or directory" during west update** | Check your internet connection and retry `west update` |
| **Flash fails on Thingy:91 X** | Verify bootloader mode (yellow LED), check USB cable supports data transfer |
| **No serial output** | Check baud rate (115200), verify correct port, try different USB cable |
| **Errors mentioning TF-M** | Make sure you're using `/ns` (non-secure) board target |
| **Build fails with Kconfig errors** | Try pristine build: `west build -p` |

For more help, see the [ATT Troubleshooting Guide](../../docs/common/tooling_troubleshooting.md).

---

## Exercise 2: Provision to nRF Cloud

The Asset Tracker Template is designed to work with nRF Cloud for data visualization, configuration, and FOTA updates. Let's connect your device!

**Objective:** Provision your device to nRF Cloud and verify successful connection and data transmission.

**Duration:** 30 minutes

### Task Description

You'll obtain a device attestation token, claim the device on nRF Cloud, and observe it provision credentials and establish a connection.

### Prerequisites

- Completed Exercise 1 (device is flashed and running)
- Active [nRF Cloud account](https://nrfcloud.com) (free tier is fine)
- SIM card installed in your device with active data plan

### Step-by-Step Instructions

#### Step 1: Get Device Attestation Token

The device needs to prove its identity to nRF Cloud. This is done via an attestation token.

1. Connect to the device via serial terminal (if not already connected)
2. Reset the device (press reset button or power cycle)
3. Watch the boot logs—on first boot, the attestation token prints automatically:

```
[00:00:02.145,324] <inf> cloud: Device is not provisioned, requesting attestation token
[00:00:02.234,252] <inf> cloud: Attestation token:
[00:00:02.234,283] <inf> cloud: %ATTESTTOKEN: "AAECAwQFBgcICQ..."
```

**If you don't see it automatically**, you can request it via shell command:

```bash
uart:~$ at at%attesttoken
```

The token will be printed in the format: `%ATTESTTOKEN: "..."`

**Copy this token** (including the quotes). You'll need it for the next step.

**Note:** This token is unique to your device and cryptographically signed by the SIM card. It cannot be forged.

#### Step 2: Claim Device on nRF Cloud

1. Log in to [nRF Cloud](https://nrfcloud.com)
2. Navigate to: **Security Services** → **Claimed Devices**
3. Click **Claim Device**
4. Paste your attestation token in the text field
5. Under "Claim rule", select **"nRF Cloud Onboarding"**

**If you don't see "nRF Cloud Onboarding" rule:**

You need to create it first:
1. Go to **Security Services** → **Claim Rules**
2. Click **Create Rule**
3. Use these settings:
   - **Name:** nRF Cloud Onboarding
   - **Gateway ID:** nRF Cloud Onboarding
   - **Type:** Thing
   - Leave other fields as default
4. Click **Create**
5. Return to claiming your device

6. Click **Claim Device**

**What's happening:**
- nRF Cloud verifies the attestation token against the SIM provider's records
- The device is registered to your account
- Provisioning credentials will be generated when the device connects

#### Step 3: Wait for Device Provisioning

After claiming, the device will automatically provision on its next connection attempt.

Watch the serial terminal:

```
[00:00:03.172,851] <dbg> cloud: state_connecting_attempt_entry: state_connecting_attempt_entry
[00:00:03.288,574] <inf> cloud: Connecting to nRF Cloud CoAP with client ID: 23437848-3230-4d7d-80ab-971ac066a8ce
[00:00:04.818,969] <inf> nrf_cloud_coap_transport: Request authorization with JWT
[00:00:05.076,232] <inf> nrf_cloud_coap_transport: Authorization result_code: 2.01
[00:00:05.076,354] <inf> nrf_cloud_coap_transport: Authorized
[00:00:05.076,568] <inf> nrf_cloud_coap_transport: DTLS CID is active
[00:00:11.381,195] <dbg> cloud: state_connected_entry: state_connected_entry
[00:00:11.381,225] <inf> cloud: Connected to Cloud
```

**Key messages to look for:**
- ✅ "Request authorization with JWT"
- ✅ "Authorization result_code: 2.01" (success)
- ✅ "Connected to Cloud"

**Timing:** Provisioning typically takes 10-30 seconds after network connection.

#### Step 4: Verify Device in nRF Cloud

1. In nRF Cloud, navigate to: **Device Management** → **Devices**
2. You should see your device listed with:
   - Device ID (IMEI or UUID)
   - Status: **Connected** (green indicator)
   - Last activity: Recent timestamp

3. Click on your device to open the device page

#### Step 5: Trigger Data Sample

Let's send some data to the cloud!

**Press Button 1** on your device:
- Thingy:91 X: The multi-function button
- nRF9151 DK: Button 1 (marked as BUTTON1)

Watch the serial logs:

```
[00:01:23.456,789] <dbg> button: button_pressed: Button 1 pressed
[00:01:23.457,012] <dbg> main: sample_data_entry: sample_data_entry
[00:01:23.457,345] <dbg> location_module: handle_location_chan: Location search trigger received
[00:01:35.123,456] <dbg> location_module: location_event_handler: Got location: lat: 63.421, lon: 10.438
[00:01:35.234,567] <dbg> environmental_module: sample_sensors: Temperature: 26.5 C, Humidity: 35.2 %
[00:01:35.345,678] <dbg> cloud: send_location: Sending location to cloud
[00:01:36.456,789] <inf> cloud: Data sent successfully
```

#### Step 6: View Data in nRF Cloud

Back in the nRF Cloud device page, you should now see:

- **Location map** with a pin showing your device's position
- **Sensor data card** with temperature, humidity, pressure
- **Battery level**
- **Timestamp** of last update

**Data should refresh when you press the button!**

### Expected Outcome

You should now have:
- ✅ Device claimed on nRF Cloud
- ✅ Device successfully provisioned and connected
- ✅ Location and sensor data visible in the cloud dashboard
- ✅ Ability to trigger sampling via button press

### Troubleshooting

| Issue | Solution |
|-------|----------|
| **Attestation token not printing** | Check that SIM card is inserted correctly. Try shell command: `at at%attesttoken` |
| **"nRF Cloud Onboarding" rule missing** | Create the claim rule manually (see instructions above) |
| **Device claims but never connects** | Check SIM has active data service. Verify network registration: `at at+cereg?` should show registered |
| **"Authorization failed" in logs** | Token may have expired. Reset device and get fresh token. Ensure device is claimed correctly. |
| **No data appears in cloud** | Press button to trigger sampling. Check serial logs for errors. Verify cloud connection status. |
| **Location shows "No fix"** | Wi-Fi scanning is used by default. Ensure Wi-Fi access points are nearby, or configure for GNSS if outdoors |

For detailed provisioning troubleshooting, see [Provisioning Documentation](../../docs/common/provisioning.md).

---

## Exercise 3: Explore the Project Structure

Understanding where things are located will help you navigate the codebase as we dive deeper in future lessons.

**Objective:** Familiarize yourself with the ATT directory structure and identify key files.

**Duration:** 20 minutes

### Task Description

You'll open the project in VS Code (or your preferred editor) and explore the organization of modules, configuration files, and documentation.

### Step-by-Step Instructions

#### Step 1: Open Project in VS Code

If using nRF Connect for VS Code:

1. Open nRF Connect extension
2. Click **Open an existing application**
3. Navigate to `asset-tracker-template/project/app`
4. Click **Open**

Alternatively, open the folder directly:
```bash
code asset-tracker-template/project/app
```

#### Step 2: Explore Module Structure

Navigate to `app/src/modules/` and open a few modules to understand their structure:

**Example: Network Module (`app/src/modules/network/`)**
- `network.h` - Public interface (message types, channel declaration)
- `network.c` - Implementation (state machine, message handling)
- `Kconfig.network` - Configuration options
- `CMakeLists.txt` - Build configuration
- `network_shell.c` - Shell commands for debugging (optional)

**Observation:** Every module follows this pattern!

#### Step 3: Examine Main Module

Open `app/src/modules/main/main.c`. This is the heart of the application logic.

**Things to notice:**
1. **State machine definition** (around line 80-150):
   ```c
   static const struct smf_state states[] = {
       [STATE_RUNNING] = SMF_CREATE_STATE(...),
       [STATE_IDLE] = SMF_CREATE_STATE(...),
       // ... more states
   };
   ```

2. **Channel subscriptions** (around line 50):
   ```c
   ZBUS_CHAN_ADD_OBS(BUTTON_CHAN, main_subscriber, 0);
   ZBUS_CHAN_ADD_OBS(LOCATION_CHAN, main_subscriber, 0);
   // ... more channels
   ```

3. **Message handling** in state run functions (around line 300+):
   ```c
   if (state_object->chan == &BUTTON_CHAN) {
       // Handle button press
   } else if (state_object->chan == &LOCATION_CHAN) {
       // Handle location data
   }
   ```

**Don't worry about understanding everything yet**—we'll cover state machines and messaging in detail in Lessons 2-4.

#### Step 4: Review Configuration Files

Open these key configuration files:

**`app/prj.conf`** - Main project configuration
- Scroll through and notice sections for:
  - Logging levels
  - Network configuration (LTE, PSM, eDRX)
  - Cloud settings
  - Module enables/disables

**`app/boards/thingy91x_nrf9151_ns.conf`** (or your board)
- Board-specific overrides
- Notice how it differs from `prj.conf`

**`app/boards/thingy91x_nrf9151_ns.overlay`**
- Device tree configuration
- Defines hardware: LEDs, buttons, sensors, I2C, SPI

#### Step 5: Browse Documentation

Open the `docs/` folder and skim through:

1. **`docs/common/architecture.md`** - Deep dive into the architecture (we'll study this in Lesson 2)
2. **`docs/common/customization.md`** - Guides for extending ATT (Lesson 5 material)
3. **`docs/modules/main.md`** - Documentation for the Main module
4. **`docs/modules/network.md`** - Documentation for the Network module

**Bookmark these**—you'll refer to them often!

#### Step 6: Identify Module Communication

Let's trace a message from button press to LED response:

1. Open `app/src/modules/button/button.h`
   - Find the `BUTTON_CHAN` channel declaration
   - Find the `button_msg` structure and message types

2. Open `app/src/modules/main/main.c`
   - Search for `BUTTON_CHAN` (Ctrl+F / Cmd+F)
   - Find where it handles `BUTTON_PRESSED` messages

3. Open `app/src/modules/led/led.h`
   - Find the `LED_CHAN` channel declaration
   - Find the `led_msg` structure

4. Back in `main.c`:
   - Find where it publishes to `LED_CHAN` to control the LED

**This is Zbus in action!** Messages flow through channels, not function calls.

### Expected Outcome

You should now:
- ✅ Understand the directory layout of ATT
- ✅ Recognize the consistent structure of modules (`.h`, `.c`, `Kconfig`, `CMakeLists.txt`)
- ✅ Know where to find module implementations, configuration, and documentation
- ✅ Have a basic sense of how modules communicate via Zbus
- ✅ Be comfortable navigating the codebase

### Discussion Questions

Take a moment to reflect:

1. **Why do you think each module has its own directory?**  
   *Hint: Think about maintainability and team development*

2. **What advantage does separating `.h` and `.c` files provide?**  
   *Hint: Consider what other modules need to know vs. implementation details*

3. **Why are there separate `prj.conf` and board-specific `.conf` files?**  
   *Hint: Think about supporting multiple hardware platforms*

We'll explore these questions more deeply in the coming lessons!

---

## Summary

Congratulations! You've completed Lesson 1. Let's recap what you've learned:

### Key Takeaways

✅ **ATT is a production-ready framework**, not just sample code  
✅ **Modular architecture** separates concerns (Network, Cloud, Sensors, etc.)  
✅ **Zbus messaging** enables loose coupling between modules  
✅ **State machines (SMF)** manage complex behaviors predictably  
✅ **nRF Cloud integration** provides ready-to-use cloud connectivity  
✅ **Device provisioning** uses secure attestation tokens from the SIM  

### Skills Acquired

- ✅ Build and flash ATT to hardware
- ✅ Provision devices to nRF Cloud
- ✅ Navigate the ATT project structure
- ✅ Identify modules and their responsibilities
- ✅ Observe message-based communication in action

### What's Next?

In **Lesson 2: Understanding the Modular Architecture**, you'll:
- Learn how modules are designed and structured
- Understand threading and task management
- Study module initialization and lifecycle
- Trace data flow through the entire system
- Dive into the responsibilities of each core module

The hands-on exercises will have you analyzing module interactions and understanding the flow of data from sensors to cloud.

---

## Additional Resources

### Documentation
- [ATT GitHub Repository](https://github.com/nrfconnect/Asset-Tracker-Template)
- [ATT Getting Started Guide](../../docs/common/getting_started.md)
- [ATT Architecture Documentation](../../docs/common/architecture.md)
- [nRF Cloud Documentation](https://docs.nordicsemi.com/bundle/nrf-cloud/page/index.html)

### NCS Resources
- [nRF Connect SDK Documentation](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/index.html)
- [Zephyr RTOS Documentation](https://docs.zephyrproject.org/latest/)
- [LTE Link Control Library](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/libraries/modem/lte_lc.html)

### Video Tutorials
- [Cellular IoT Fundamentals Course](https://academy.nordicsemi.com/courses/cellular-iot-fundamentals/)
- [nRF Connect SDK Fundamentals](https://academy.nordicsemi.com/courses/nrf-connect-sdk-fundamentals/)

### Community Support
- [Nordic DevZone](https://devzone.nordicsemi.com) - Ask questions, get help
- [GitHub Issues](https://github.com/nrfconnect/Asset-Tracker-Template/issues) - Report bugs, request features

---

**Ready for Lesson 2?** → [Understanding the Modular Architecture](../lesson-2/lesson.md)

