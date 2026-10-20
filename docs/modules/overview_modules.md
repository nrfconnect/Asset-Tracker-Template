# Modules

The Asset Tracker Template is organized into modules, each responsible for a specific functionality. The core modules run on all supported hardware, and a second set of modules is specific to the Thingy:91 X.

## Core modules

The template provides the following core modules:

| Module | Description |
|--------|-------------|
| [Main](main.md) | Implements the business logic and controls the overall application behavior. |
| [Storage](storage.md) | Stores data from enabled modules. |
| [Network](network.md) | Manages LTE connectivity and tracks network status. |
| [Cloud](cloud.md) | Handles communication with nRF Cloud using the Constrained Application Protocol (CoAP). |
| [Location](location.md) | Provides location services using Global Navigation Satellite System (GNSS), Wi-Fi, and cellular positioning. |
| [Button](button.md) | Reports button press events for user input. |
| [FOTA](fota_module.md) | Manages firmware over-the-air updates. |

## Thingy:91 X specific modules

The following modules are specific to the Thingy:91 X:

| Module | Description |
|--------|-------------|
| [Environmental](environmental.md) | Collects environmental sensor data (temperature, humidity, pressure). |
| [LED](led.md) | Controls an RGB LED for visual indication. |
| [Power](power.md) | Monitors battery status and provides power management. |
| [UART Power Control](uart_power_control.md) | Suspends and resumes the UART when VBUS changes. |
