# Network module

The Network module manages the cellular connectivity for applications running on nRF91 Series devices. It handles network connection states, system mode configuration, and network power saving features such as extended discontinuous reception (eDRX) and power saving mode (PSM). It uses the [LTE Link Control](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/libraries/modem/lte_lc.html) library from the nRF Connect SDK to control the modem and monitor network events. Internally, the module implements a state machine that uses Zephyr's [State Machine Framework](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/smf/index.html).

By default, the module automatically searches for a suitable network on startup and maintains the connection for the lifetime of the application.
You can also configure the library to be fully controlled by the application instead, giving closer control over the LTE link and current consumption.
For more information, see [Configuration](#configuration).

## Architecture

The Network module implements a state machine with the following states and transitions:

![Network module state diagram](../images/network_module_state_diagram.svg)

## Messages

The Network module communicates through the zbus channel `network_chan`.
The messages are defined in `network.h`. A message consists of a type and optional data. Each message is either an input message or an output message.
All input messages are requests from the application to the Network module. The output messages may be responses to input messages or notifications from the Network module to the application.

### Input messages

The module accepts the following input messages:

- **NETWORK_CONNECT**: Request to connect to the network.
- **NETWORK_DISCONNECT**: Request to disconnect from the network.
- **NETWORK_SEARCH_STOP**: Stop searching for a network. The module does not attempt to connect to a network until it receives a new `NETWORK_CONNECT` message.
- **NETWORK_SYSTEM_MODE_REQUEST**: Request to retrieve the current system mode. The module sends the response as a `NETWORK_SYSTEM_MODE_RESPONSE` message.
- **NETWORK_SYSTEM_MODE_SET_LTEM**: Request to set the system mode to use LTE-M only.
- **NETWORK_SYSTEM_MODE_SET_NBIOT**: Request to set the system mode to use NB-IoT only.
- **NETWORK_SYSTEM_MODE_SET_LTEM_NBIOT**: Request to set the system mode to use both LTE-M and NB-IoT.

### Output messages

The module publishes the following output messages:

- **NETWORK_DISCONNECTED**: The device is disconnected from the network.
- **NETWORK_CONNECTED**: The device is connected to the network and has an IP address.
- **NETWORK_MODEM_RESET_LOOP**: The modem has detected a reset loop with too many attach requests within a short time.
- **NETWORK_UICC_FAILURE**: The modem has detected an error with the SIM card. Confirm that it is installed correctly.
- **NETWORK_LIGHT_SEARCH_DONE**: The modem has completed a light search based on previous cell history without finding a suitable cell. This message can be used to stop the search to save power.
- **NETWORK_ATTACH_REJECTED**: A network attach request has been rejected by the network.
- **NETWORK_PSM_PARAMS**: PSM parameters have been received, in the `.psm_cfg` field of the message.
- **NETWORK_EDRX_PARAMS**: eDRX parameters have been received, in the `.edrx_cfg` field of the message.
- **NETWORK_SYSTEM_MODE_RESPONSE**: Response to a `NETWORK_SYSTEM_MODE_REQUEST` message, with the current mode in the `.system_mode` field.

### Message structure

The Network module uses the `struct network_msg` structure for communication:

```c
struct network_msg {
    enum network_msg_type type;
    union {
        enum lte_lc_system_mode system_mode;
        struct lte_lc_psm_cfg psm_cfg;
        struct lte_lc_edrx_cfg edrx_cfg;
    };
};
```

## Configuration

You can configure the Network module with the following Kconfig options:

- **CONFIG_APP_NETWORK_THREAD_STACK_SIZE**: Sets the stack size for the Network module thread.
- **CONFIG_APP_NETWORK_WATCHDOG_TIMEOUT_SECONDS**: Defines the timeout in seconds for the Network module watchdog. This timeout covers both waiting for incoming messages and message processing time.
- **CONFIG_APP_NETWORK_MSG_PROCESSING_TIMEOUT_SECONDS**: Sets the maximum time allowed for processing a single message in the module's state machine. This value must be smaller than the watchdog timeout.
- **CONFIG_APP_NETWORK_SEARCH_NETWORK_ON_STARTUP**: When enabled, the module automatically searches for a network on startup. When disabled, a `NETWORK_CONNECT` message triggers the network search.
- **CONFIG_APP_NETWORK_LOG_LEVEL_***: Controls the logging level for the Network module. This follows Zephyr's standard logging configuration pattern.
