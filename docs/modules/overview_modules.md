# Modules

## Core modules

| Module | Description |
|--------|-------------|
| [Main](main.md) | Implements the business logic and controls the overall application behavior. |
| [Storage](storage.md) | Stores data from enabled modules. |
| [Network](network.md) | Manages LTE connectivity and tracks network status. |
| [Cloud](cloud.md) | Handles communication with nRF Cloud using CoAP. |
| [Location](location.md) | Provides location services using GNSS, Wi-Fi, and cellular positioning. |
| [Button](button.md) | Reports button press events for user input. |
| [FOTA](fota_module.md) | Manages firmware over-the-air updates. |

## Thingy:91 X specific modules

| Module | Description |
|--------|-------------|
| [Environmental](environmental.md) | Collects environmental sensor data (temperature, humidity, pressure). |
| [LED](led.md) | Controls an RGB LED for visual indication. |
| [Power](power.md) | Monitors battery status and provides power management. |
| [UART Power Control](uart_power_control.md) | UART suspend/resume on VBUS changes. |
