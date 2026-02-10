# Main module

The Main module serves as the central control unit of the Asset Tracker Template. It implements a hierarchical state machine that coordinates the activities of all other modules through [zbus](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/zbus/index.html) messages.
This module handles the application's business logic, including cloud connectivity, data sampling, firmware updates, configuration updates, and user interactions.

## Architecture

### State diagram

The Main module implements a state machine with the following states and transitions:

![Main module state diagram](../images/main_module_state_diagram.svg "Main module state diagram")

## Messages

The main module does not implement messages available to other modules. Instead, it processes messages from other modules to control the application's behavior.
It subscribes to messages on the following zbus channels:

The Main module uses the following zbus channels, both for subscribing to incoming data and publishing outbound requests or status updates:

| Zbus Channels          | Description                                                                                |
|------------------------|--------------------------------------------------------------------------------------------|
| **BUTTON_CHAN**        | Process user button presses to trigger data samples or sending.                            |
| **CLOUD_CHAN**         | Receive connectivity status and cloud responses. Trigger device shadow polling.            |
| **STORAGE_CHAN**       | Control the storage module and receive status responses.                                   |
| **ENVIRONMENTAL_CHAN** | Request sensor data from the environmental module.                                         |
| **FOTA_CHAN**          | Poll for FOTA updates, manage the FOTA process, and apply updates.                         |
| **LED_CHAN**           | Update LED patterns to indicate system state.                                              |
| **LOCATION_CHAN**      | Request new location data when samples are due.                                            |
| **NETWORK_CHAN**       | Control LTE network connection and track cellular connectivity events.                     |
| **POWER_CHAN**         | Request battery status and initiate low-power mode.                                        |
| **TIMER_CHAN**         | Handle timer events for sampling.                                                          |

## Configuration

The Main module can be configured using the following Kconfig options:

* **CONFIG_APP_LOG_LEVEL:**
  Controls logging level for the main module.

* **CONFIG_APP_BUFFER_MODE_SAMPLING_INTERVAL_SECONDS:**
  Default sensor data sampling interval in buffer mode. Triggers sensor sampling and location search.

* **CONFIG_APP_CLOUD_UPDATE_INTERVAL_SECONDS:**
  Interval for cloud synchronization activities, including polling and data sending. Triggers cloud shadow and FOTA status polling.
  In passthrough mode, this option also controls how often data is sampled and sent to cloud.

* **CONFIG_APP_MSG_PROCESSING_TIMEOUT_SECONDS:**
  Maximum time allowed for processing a single message.

* **CONFIG_APP_WATCHDOG_TIMEOUT_SECONDS:**
  Defines the watchdog timeout for the main module.
