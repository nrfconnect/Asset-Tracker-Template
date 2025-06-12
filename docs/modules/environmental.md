# Environmental module

The Environmental module collects sensor data from the onboard BME680 environmental sensor on the Thingy:91 X.
The module provides temperature, pressure, and humidity readings to other modules through the zbus messaging system. The module initializes the sensor hardware and responds to sampling requests from the main application module.

To initiate a sensor reading, the main module sends an `ENVIRONMENTAL_SENSOR_SAMPLE_REQUEST` message to the environmental module. The environmental module then reads the sensor data and sends an `ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE` message with the collected data asynchronously.

## Messages

The Environmental module communicates via the zbus channel `ENVIRONMENTAL_CHAN`, using message types defined in `environmental.h`.

### Input Messages

- **ENVIRONMENTAL_SENSOR_SAMPLE_REQUEST:**
  Requests the module to take new sensor readings from the environmental sensor.

### Output Messages

- **ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE:**
  Returns collected environmental data with the following fields:

  - `temperature`: Temperature value in degrees Celsius.
  - `pressure`: Atmospheric pressure in Pascals.
  - `humidity`: Relative humidity percentage.

The message structure used by the environmental module is defined in `environmental.h`:

```c
struct environmental_msg {
    enum environmental_msg_type type;
    double temperature;
    double pressure;
    double humidity;
};
```

## Configuration
The Environmental module can be configured using the following Kconfig options:

- **CONFIG_APP_ENVIRONMENTAL:**
  Enables the Environmental module.

- **CONFIG_APP_ENVIRONMENTAL_THREAD_STACK_SIZE:**
  Size of the stack for the environmental module's thread.

- **CONFIG_APP_ENVIRONMENTAL_WATCHDOG_TIMEOUT_SECONDS:**
  Defines the watchdog timeout for the environmental module.

- **CONFIG_APP_ENVIRONMENTAL_MSG_PROCESSING_TIMEOUT_SECONDS:**
  Maximum time allowed for processing a single message.

- **CONFIG_APP_ENVIRONMENTAL_LOG_LEVEL_INF/_ERR/_WRN/_DBG:**
  Controls logging level for the environmental module.

For more details on Kconfig options, see the `Kconfig.environmental` file in the module's directory.

## State diagram

The environmental module implements a very simple state machine with only one state:

```mermaid
stateDiagram-v2
    [*] --> STATE_RUNNING
```
