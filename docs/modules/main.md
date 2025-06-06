# Main module

The Main module serves as the central control unit of the Asset Tracker Template. It implements a hierarchical state machine that coordinates the activities of all other modules through [zbus](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/zbus/index.html) messages.
This module handles the application's business logic, including cloud connectivity, data sampling, firmware updates, configuration updates, and user interactions.

## Messages

The main module does not implement messages available to other modules. Instead, it processes messages from other modules to control the application's behavior.
It subscribes to messages on the following zbus channels:

The Main module uses the following zbus channels, both for subscribing to incoming data and publishing outbound requests or status updates:

* **BUTTON_CHAN**
  - Processes user button presses for manually triggering data samples.

* **CLOUD_CHAN**
  - Receive connectivity status (connected, disconnected) and cloud response data.
  - Trigger device shadow polling to retrieve configuration updates.

* **ENVIRONMENTAL_CHAN**
  - Request sensor data from the environmental module.

* **FOTA_CHAN**
  - Poll for FOTA updates and manage the FOTA process.
  - Apply FOTA updates to install the new firmware image.

* **LED_CHAN**
  - Update LED pattern to indicate system state.

* **LOCATION_CHAN**
  - Requests new location data when a sample is due.

* **NETWORK_CHAN**
  - Control LTE network connection.
  - Track cellular connectivity events.
  - Request network quality samples.

* **POWER_CHAN**
  - Request battery status.
  - Initiate low-power mode.

* **TIMER_CHAN**
  - Handle timer events for sampling.


## Configuration

The Main module can be configured using the following Kconfig options:

* **CONFIG_APP_LOG_LEVEL**
  Controls logging level for the main module.

* **CONFIG_APP_MODULE_TRIGGER_TIMEOUT_SECONDS**
  Default data sampling interval.

* **CONFIG_APP_REQUEST_NETWORK_QUALITY**
  When enabled, requests network quality metrics during regular sampling.

* **CONFIG_APP_MSG_PROCESSING_TIMEOUT_SECONDS**
  Maximum time allowed for processing a single message.

* **CONFIG_APP_WATCHDOG_TIMEOUT_SECONDS**
  Defines the watchdog timeout for the main module.


## State Diagram

The Main module implements a hierarchical state machine with the following states:

```mermaid
stateDiagram-v2
    [*] --> STATE_RUNNING

    state STATE_RUNNING {
        [*] --> STATE_IDLE

        STATE_IDLE --> STATE_TRIGGERING : CLOUD_CONNECTED_READY_TO_SEND
        STATE_TRIGGERING --> STATE_IDLE : CLOUD_DISCONNECTED/CLOUD_CONNECTED_PAUSED

        state STATE_TRIGGERING {
            [*] --> STATE_SAMPLE_DATA
            STATE_WAIT_FOR_TRIGGER --> STATE_SAMPLE_DATA : timer_trigger/button_press

            STATE_SAMPLE_DATA --> STATE_WAIT_FOR_TRIGGER : LOCATION_SEARCH_DONE
        }
    }

    STATE_RUNNING --> STATE_FOTA : FOTA_DOWNLOADING_UPDATE

    state STATE_FOTA {
        [*] --> STATE_FOTA_DOWNLOADING

        STATE_FOTA_DOWNLOADING --> STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT : FOTA_SUCCESS_REBOOT_NEEDED
        STATE_FOTA_DOWNLOADING --> STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT_TO_APPLY_IMAGE : FOTA_IMAGE_APPLY_NEEDED

        STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT --> STATE_FOTA_REBOOTING : NETWORK_DISCONNECTED

        STATE_FOTA_WAITING_FOR_NETWORK_DISCONNECT_TO_APPLY_IMAGE --> STATE_FOTA_APPLYING_IMAGE : NETWORK_DISCONNECTED
        STATE_FOTA_APPLYING_IMAGE --> STATE_FOTA_REBOOTING : FOTA_SUCCESS_REBOOT_NEEDED

        STATE_FOTA_REBOOTING --> [*] : sys_reboot()
    }

    STATE_FOTA --> STATE_RUNNING : FOTA_DOWNLOAD_CANCELED/TIMED_OUT/FAILED
```
