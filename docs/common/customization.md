# Customization

This guide explains modifying the specific aspects of the template.

- [Add a new zbus event](#add-a-new-zbus-event)
- [Add environmental sensor](#add-environmental-sensor)
- [Add your own module](#add-your-own-module)
- [Enable support for MQTT](#enable-support-for-mqtt)

## Add a new zbus event

This section demonstrates how to add a new event to a module and utilize it in another module within the system. In this example, you can add events to the power module to notify the system when VBUS is connected or disconnected on the Thingy:91 X.
The main module subscribes to these events and request specific LED patterns from the LED module in response.

When VBUS is connected, the LED will toggle white for 10 seconds.
When VBUS is disconnected, the LED will toggle purple for 10 seconds.

To apply all the necessary changes to the template, use the following command:

```bash
git apply <path-to-template-dir>/Asset-Tracker-Template/patches/add-event.patch
```

### Instructions

To add a new zbus event, complete the following procedure:

1. Define the new events in your module's header file (for example, `power.h`):

    ```c
    enum power_msg_type {
       /* ... existing message types ... */

       /* VBUS power supply is connected. */
       POWER_VBUS_CONNECTED,

       /* VBUS power supply is disconnected. */
       POWER_VBUS_DISCONNECTED,
    };
    ```

1. Implement publishing VBUS connected/disconnected events in the appropiate handler in `power.c: event_callback()`:

    ```c
    if (pins & BIT(NPM1300_EVENT_VBUS_DETECTED)) {
        LOG_DBG("VBUS detected");

        enum power_msg_type msg = POWER_VBUS_CONNECTED;

        err = zbus_chan_pub(&POWER_CHAN, &msg, K_SECONDS(1));
        if (err) {
            LOG_ERR("zbus_chan_pub, error: %d", err);
            SEND_FATAL_ERROR();
            return;
        }
        // ... existing code ...
        }

    if (pins & BIT(NPM1300_EVENT_VBUS_REMOVED)) {
        LOG_DBG("VBUS removed");

        enum power_msg_type msg = POWER_VBUS_DISCONNECTED;

        err = zbus_chan_pub(&POWER_CHAN, &msg, K_SECONDS(1));
        if (err) {
            LOG_ERR("zbus_chan_pub, error: %d", err);
            SEND_FATAL_ERROR();
            return;
        }

        // ... existing code ...
    }
    ```

1. Make sure the channel is included in the subscriber module (for example, `main.c`). Add the channel to the channel list:

    ```c
    #define LIST_OF_CHANNELS(X)                          \
        X(LED_CHAN,		    enum led_msg_type)          \
        X(LOCATION_CHAN,		enum location_msg_type)     \
        X(POWER_CHAN,		enum power_msg_type)	    \
        X(TIMER_CHAN,		int)
    ```

1. Implement a handler for the new events in the subscriber module (for example, in the main module's state machine in `running_run`):

    ```c
    if (state_object->chan == &POWER_CHAN) {
        struct power_msg msg = MSG_TO_POWER_MSG(state_object->msg_buf);

        if (msg.type == POWER_VBUS_CONNECTED) {
            LOG_WRN("VBUS connected, request white LED blinking 10 seconds");

            struct led_msg led_msg = {
                .type = LED_RGB_SET,
                .red = 255,
                .green = 255,
                .blue = 255,
                .duration_on_msec = 1000,
                .duration_off_msec = 700,
                .repetitions = 10,
            };

            int err = zbus_chan_pub(&LED_CHAN, &led_msg, K_SECONDS(1));

            if (err) {
                LOG_ERR("zbus_chan_pub, error: %d", err);
                SEND_FATAL_ERROR();
                return;
            }

            return;
        } else if (msg.type == POWER_VBUS_DISCONNECTED) {
            LOG_WRN("VBUS disconnected, request purple LED blinking for 10 seconds");

            struct led_msg led_msg = {
                .type = LED_RGB_SET,
                .red = 255,
                .green = 0,
                .blue = 255,
                .duration_on_msec = 1000,
                .duration_off_msec = 700,
                .repetitions = 10,
            };

            int err = zbus_chan_pub(&LED_CHAN, &led_msg, K_SECONDS(1));

            if (err) {
                LOG_ERR("zbus_chan_pub, error: %d", err);
                SEND_FATAL_ERROR();
                return;
            }

            return;
        }
    }
    ```
1. Test the implementation by connecting and disconnecting VBUS to verify the LED patterns change as expected.

## Add environmental sensor

This section demonstrates how to add support for the BMM350 magnetometer.
The environmental module will be updated to sample data from the sensor through the Zephyr Sensor API.
The data is forwarded to nRF Cloud along with all the other data types sampled by the system.

To add basic support for the BMM350 magnetometer to the template, use the following command:

```bash
git apply <path-to-template-dir>/Asset-Tracker-Template/patches/magnetometer.patch
```

### Instructions

To add a new sensor to the environmental module, ensure that:

1. The sensor is properly configured in the Device Tree (DTS).
1. The corresponding driver is available in the Zephyr RTOS.
1. The sensor is compatible with the Zephyr Sensor API.

In this example, support for the Bosch BMM350 Magnetometer sensor is added using the [Zephyr Sensor API](https://docs.zephyrproject.org/latest/hardware/peripherals/sensor/index.html). The BMM350 driver in Zephyr integrates with the Sensor API.
This applies in general for all sensors that support the Zephyr Sensor API.

Thingy:91 X is used as an example, as it is a supported board in the template with defined board files in the nRF Connect SDK.

1. Enable the sensor in the devicetree by setting its status to "okay". This will perform the following:

   - Instantiate the devicetree node for the sensor.
   - Initialize the driver during boot.
   - Make the sensor ready for use.

    Add the following to the board-specific devicetree overlay file (`thingy91x_nrf9151_ns.overlay`):

    ```c
    &magnetometer {
        status = "okay";
    };
    ```

1. Update the environmental module's state structure to include the magnetometer device reference and data storage:

    ```c
    struct environmental_state {
        /* ... existing fields ... */

       /* BMM350 sensor device reference */
        const struct device *const bmm350;

        /* Magnetic field measurements (X, Y, Z) in gauss */
        double magnetic_field[3];

        /* ... existing fields ... */
    };
    ```

1. Initialize the device reference using the devicetree label:

    ```c
    struct environmental_state environmental_state = {
        .bme680 = DEVICE_DT_GET(DT_NODELABEL(bme680)),
        .bmm350 = DEVICE_DT_GET(DT_NODELABEL(magnetometer)),
    };
    ```

1. Update the sensor sampling function signature to include the magnetometer device:

     ```c
     static void sample_sensors(const struct device *const bme680, const struct device *const bmm350)
    ```

   And update the function call:

    ```c
     sample_sensors(state_object->bme680, state_object->bmm350);
    ```

1. Implement sensor data acquisition using the Zephyr Sensor API:

     ```c
     err = sensor_sample_fetch(bmm350);
    if (err) {
        LOG_ERR("Failed to fetch magnetometer sample: %d", err);
        SEND_FATAL_ERROR();
        return;
    }

    err = sensor_channel_get(bmm350, SENSOR_CHAN_MAGN_XYZ, &magnetic_field);
     if (err) {
        LOG_ERR("Failed to get magnetometer data: %d", err);
        SEND_FATAL_ERROR();
        return;
    }

    LOG_DBG("Magnetic field: X: %.2f µT, Y: %.2f µT, Z: %.2f µT",
            sensor_value_to_double(&magnetic_field[0]),
            sensor_value_to_double(&magnetic_field[1]),
            sensor_value_to_double(&magnetic_field[2]));

    struct environmental_msg msg = {
        .type = ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE,
        .temperature = sensor_value_to_double(&temp),
        .pressure = sensor_value_to_double(&press),
        .humidity = sensor_value_to_double(&humidity),
        .magnetic_field[0] = sensor_value_to_double(&magnetic_field[0]),
        .magnetic_field[1] = sensor_value_to_double(&magnetic_field[1]),
        .magnetic_field[2] = sensor_value_to_double(&magnetic_field[2]),
    };
    ```

1. Add cloud integration for the magnetometer data:

    ```c
    char message[100] = { 0 };

    err = snprintk(message, sizeof(message),
                "%.2f %.2f %.2f",
                msg.magnetic_field[0],
                msg.magnetic_field[1],
                msg.magnetic_field[2]);
    if (err < 0 || err >= sizeof(message)) {
        LOG_ERR("Failed to format magnetometer data: %d", err);
        SEND_FATAL_ERROR();
        return;
    }

    err = nrf_cloud_coap_message_send(CUSTOM_JSON_APPID_VAL_MAGNETIC,
                                  message,
                                  false,
                                  NRF_CLOUD_NO_TIMESTAMP,
                                  confirmable);
    if (err == -ENETUNREACH) {
        LOG_WRN("Network unreachable: %d", err);
        return;
    } else if (err) {
        LOG_ERR("Failed to send magnetometer data: %d", err);
        SEND_FATAL_ERROR();
        return;
    }
    ```

1. Define a custom APP ID for magnetic field data:

    ```c
    #define CUSTOM_JSON_APPID_VAL_MAGNETIC "MAGNETIC_FIELD"
    ```

## Add your own module

The dummy module serves as a template for understanding the module architecture and can be used as a foundation for custom modules.

To add the dummy module to the template, apply the following patch:

```bash
git apply <path-to-template-dir>/Asset-Tracker-Template/patches/dummy-module.patch
```

If you want to generate and apply a dummy module with a custom name other than "Dummy", you can run the following script to rename the module and apply the patch:

```bash
cd Asset-Tracker-Template
```

```bash
python3 scripts/rename_patch.py
```

Follow the instructions to rename and apply the patch:

```bash
===== Patch Module Renamer =====

Enter the new module name (e.g. new-name): accelerometer
Enter the full path to the patch file: /<path-to-template-dir>/Asset-Tracker-Template/patches/dummy-module.patch

Patched file written to: /tmp/accelerometer-module.patch
Do you want to apply the changes now with 'git apply'? (y/N): y
Patch applied successfully.
```

### Instructions

To add your own module, complete the following steps:

1. Create the module directory structure:

    ```bash
    mkdir -p app/src/modules/dummy
    ```

2. Create the following files in the module directory:

   - `dummy.h` - Module interface definitions.
   - `dummy.c` - Module implementation.
   - `Kconfig.dummy` - Module configuration options.
   - `CMakeLists.txt` - Build system configuration.

3. In `dummy.h`, define the module's interface:

    ```c
    #ifndef _DUMMY_H_
    #define _DUMMY_H_

    #include <zephyr/kernel.h>
    #include <zephyr/zbus/zbus.h>

    #ifdef __cplusplus
    extern "C" {
    #endif

    /* Module's zbus channel */
    ZBUS_CHAN_DECLARE(DUMMY_CHAN);

    /* Module message types */
    enum dummy_msg_type {
        /* Output message types */
        DUMMY_SAMPLE_RESPONSE = 0x1,

        /* Input message types */
        DUMMY_SAMPLE_REQUEST,
    };

    /* Module message structure */
    struct dummy_msg {
        enum dummy_msg_type type;
        int32_t value;
    };

    #define MSG_TO_DUMMY_MSG(_msg) (*(const struct dummy_msg *)_msg)

    #ifdef __cplusplus
    }
    #endif

    #endif /* _DUMMY_H_ */
    ```

1. In `dummy.c`, implement the module's functionality:

    ```c
    #include <zephyr/kernel.h>
    #include <zephyr/logging/log.h>
    #include <zephyr/zbus/zbus.h>
    #include <zephyr/task_wdt/task_wdt.h>
    #include <zephyr/smf.h>

    #include "app_common.h"
    #include "dummy.h"

    /* Register log module */
    LOG_MODULE_REGISTER(dummy_module, CONFIG_APP_DUMMY_LOG_LEVEL);

    /* Define module's zbus channel */
    ZBUS_CHAN_DEFINE(DUMMY_CHAN,
                     struct dummy_msg,
                     NULL,
                     NULL,
                     ZBUS_OBSERVERS_EMPTY,
                     ZBUS_MSG_INIT(0)
    );

    /* Register zbus subscriber */
    ZBUS_MSG_SUBSCRIBER_DEFINE(dummy);

    /* Add subscriber to channel */
    ZBUS_CHAN_ADD_OBS(DUMMY_CHAN, dummy, 0);

    #define MAX_MSG_SIZE sizeof(struct dummy_msg)

    BUILD_ASSERT(CONFIG_APP_DUMMY_WATCHDOG_TIMEOUT_SECONDS >
                 CONFIG_APP_DUMMY_MSG_PROCESSING_TIMEOUT_SECONDS,
                 "Watchdog timeout must be greater than maximum message processing time");

    /* State machine states */
    enum dummy_module_state {
        STATE_RUNNING,
    };

    /* Module state structure */
    struct dummy_state {
        /* State machine context (must be first) */
        struct smf_ctx ctx;

        /* Last received zbus channel */
        const struct zbus_channel *chan;

        /* Message buffer */
        uint8_t msg_buf[MAX_MSG_SIZE];

        /* Current counter value */
        int32_t current_value;
    };

    /* Forward declarations */
    static void state_running_run(void *o);

    /* State machine definition */
    static const struct smf_state states[] = {
        [STATE_RUNNING] = SMF_CREATE_STATE(NULL, state_running_run, NULL, NULL, NULL),
    };

    /* Watchdog callback */
    static void task_wdt_callback(int channel_id, void *user_data)
    {
        LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
                channel_id, k_thread_name_get((k_tid_t)user_data));

        SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
    }

    /* State machine handlers */
    static void state_running_run(void *o)
    {
        const struct dummy_state *state_object = (const struct dummy_state *)o;

        if (&DUMMY_CHAN == state_object->chan) {
            struct dummy_msg msg = MSG_TO_DUMMY_MSG(state_object->msg_buf);

            if (msg.type == DUMMY_SAMPLE_REQUEST) {
                LOG_DBG("Received sample request");
                state_object->current_value++;

                struct dummy_msg response = {
                    .type = DUMMY_SAMPLE_RESPONSE,
                    .value = state_object->current_value
                };

                int err = zbus_chan_pub(&DUMMY_CHAN, &response, K_NO_WAIT);
                if (err) {
                    LOG_ERR("Failed to publish response: %d", err);
                    SEND_FATAL_ERROR();
                    return;
                }
            }
        }
    }

    /* Module task function */
    static void dummy_task(void)
    {
        int err;
        int task_wdt_id;
        const uint32_t wdt_timeout_ms =
            (CONFIG_APP_DUMMY_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
        const uint32_t execution_time_ms =
            (CONFIG_APP_DUMMY_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
        const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
        struct dummy_state dummy_state = {
            .current_value = 0
        };

        LOG_DBG("Starting dummy module task");

        task_wdt_id = task_wdt_add(wdt_timeout_ms, task_wdt_callback, (void *)k_current_get());

        smf_set_initial(SMF_CTX(&dummy_state), &states[STATE_RUNNING]);

        while (true) {
            err = task_wdt_feed(task_wdt_id);
            if (err) {
                LOG_ERR("Failed to feed watchdog: %d", err);
                SEND_FATAL_ERROR();
                return;
            }

            err = zbus_sub_wait_msg(&dummy,
                                   &dummy_state.chan,
                                   dummy_state.msg_buf,
                                   zbus_wait_ms);
            if (err == -ENOMSG) {
                continue;
            } else if (err) {
                LOG_ERR("Failed to wait for message: %d", err);
                SEND_FATAL_ERROR();
                return;
            }

            err = smf_run_state(SMF_CTX(&dummy_state));
            if (err) {
                LOG_ERR("Failed to run state machine: %d", err);
                SEND_FATAL_ERROR();
                return;
            }
        }
    }

    /* Define module thread */
    K_THREAD_DEFINE(dummy_task_id,
                    CONFIG_APP_DUMMY_THREAD_STACK_SIZE,
                    dummy_task, NULL, NULL, NULL,
                    K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
    ```

1. In `Kconfig.dummy`, define module configuration options:

    ```kconfig
    menuconfig APP_DUMMY
        bool "Dummy module"
        default y
        help
            Enable the dummy module.

    if APP_DUMMY

    config APP_DUMMY_THREAD_STACK_SIZE
        int "Dummy module thread stack size"
        default 2048
        help
            Stack size for the dummy module thread.

    config APP_DUMMY_WATCHDOG_TIMEOUT_SECONDS
        int "Dummy module watchdog timeout in seconds"
        default 30
        help
            Watchdog timeout for the dummy module.

    config APP_DUMMY_MSG_PROCESSING_TIMEOUT_SECONDS
        int "Dummy module message processing timeout in seconds"
        default 5
        help
            Maximum time allowed for processing a single message in the dummy module.

    module = APP_DUMMY
    module-str = DUMMY
    source "subsys/logging/Kconfig.template.log_config"

    endif # APP_DUMMY
    ```

1. In `CMakeLists.txt`, configure the build system:

    ```cmake
    target_sources_ifdef(CONFIG_APP_DUMMY app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/dummy.c)
    target_include_directories(app PRIVATE .)
    ```

1. Add the module to the main application's CMakeLists.txt:

    ```cmake
    add_subdirectory(src/modules/dummy)
    ```

The dummy module is now ready to use. It provides the following functionality:

- Initializes with a counter value of `0`.
- Increments the counter on each sample request.
- Responds with the current counter value using zbus.
- Includes error handling and watchdog support.
- Follows the state machine pattern used by other modules.

To test the module, send a `DUMMY_SAMPLE_REQUEST` message to its zbus channel. The module responds with a `DUMMY_SAMPLE_RESPONSE` containing the incremented counter value.

This dummy module serves as a template that you can extend to implement more complex functionality. You can add additional message types, state variables, and processing logic as needed for your specific use case.

## Enable support for MQTT

To connect to a generic MQTT server using the Asset Tracker Template, you can use the example cloud module provided under `examples/modules/cloud`. This module replaces the default nRF Cloud CoAP-based cloud integration with a flexible MQTT client implementation.

- **MQTT module *default* configurations:**

    - **Broker hostname:** [mqtt.nordicsemi.academy](https://mqtt.nordicsemi.academy/)
    - **Device/Client ID** IMEI (International Mobile Equipment Identity)
    - **Port:** 8883
    - **TLS:** Yes
    - **Authentication:** Server only
    - **CA:** modules/examples/cloud/creds/mqtt.nordicsemi.academy.pem
    - **Subscribed topic** imei/att-pub-topic
    - **Publishing toptic** imei/att-sub-topic

### Configuration

Configurations for the MQTT stack can be set in the `overlay-mqtt.conf` file and Kconfig options defined in `examples/modules/cloud/Kconfig.cloud_mqtt`.
The following are some of the available options for controlling the MQTT module:

- `CONFIG_APP_CLOUD_MQTT`
- `CONFIG_APP_CLOUD_MQTT_HOSTNAME`
- `CONFIG_APP_CLOUD_MQTT_TOPIC_SIZE_MAX`
- `CONFIG_APP_CLOUD_MQTT_PUB_TOPIC`
- `CONFIG_APP_CLOUD_MQTT_SUB_TOPIC`
- `CONFIG_APP_CLOUD_MQTT_SEC_TAG`
- `CONFIG_APP_CLOUD_MQTT_SHELL`
- `CONFIG_APP_CLOUD_MQTT_PAYLOAD_BUFFER_MAX_SIZE`
- `CONFIG_APP_CLOUD_MQTT_SHADOW_RESPONSE_BUFFER_MAX_SIZE`
- `CONFIG_APP_CLOUD_MQTT_BACKOFF_INITIAL_SECONDS`
- `CONFIG_APP_CLOUD_MQTT_BACKOFF_TYPE_LINEAR`
- `CONFIG_APP_CLOUD_MQTT_BACKOFF_TYPE_EXPONENTIAL`
- `CONFIG_APP_CLOUD_MQTT_BACKOFF_TYPE_NONE`
- `CONFIG_APP_CLOUD_MQTT_BACKOFF_LINEAR_INCREMENT_SECONDS`
- `CONFIG_APP_CLOUD_MQTT_BACKOFF_MAX_SECONDS`
- `CONFIG_APP_CLOUD_MQTT_THREAD_STACK_SIZE`
- `CONFIG_APP_CLOUD_MQTT_MESSAGE_QUEUE_SIZE`
- `CONFIG_APP_CLOUD_MQTT_WATCHDOG_TIMEOUT_SECONDS`
- `CONFIG_APP_CLOUD_MQTT_MSG_PROCESSING_TIMEOUT_SECONDS`

### How to use the MQTT Cloud Example

1. Build and flash with the MQTT overlay.

   In the template's `app` folder, run:

   ```sh
   west build -p -b thingy91x/nrf9151/ns -- -DEXTRA_CONF_FILE="$(pwd)/../examples/modules/cloud/overlay-mqtt.conf" && west flash --erase --skip-rebuild
   ```

1. Observe that the device connects to the broker.

1. Test using shell commands:

    ```bash
    uart:~$ att_cloud_publish_mqtt test-payload
    Sending on payload channel: "data":"test-payload","ts":1746534066186 (40 bytes)
    [00:00:18.607,421] <dbg> cloud: on_cloud_payload_json: MQTT Publish Details:
    [00:00:18.607,482] <dbg> cloud: on_cloud_payload_json:  -Payload: "data":"test-payload","ts":1746534066186
    [00:00:18.607,513] <dbg> cloud: on_cloud_payload_json:  -Payload Length: 40
    [00:00:18.607,543] <dbg> cloud: on_cloud_payload_json:  -Topic: 359404230261381/att-pub-topic
    [00:00:18.607,574] <dbg> cloud: on_cloud_payload_json:  -Topic Size: 29
    [00:00:18.607,635] <dbg> cloud: on_cloud_payload_json:  -QoS: 1
    [00:00:18.607,635] <dbg> cloud: on_cloud_payload_json:  -Message ID: 1
    [00:00:18.607,696] <dbg> mqtt_helper: mqtt_helper_publish: Publishing to topic: 359404230261381/att-pub-topic
    [00:00:19.141,235] <dbg> mqtt_helper: mqtt_evt_handler: MQTT_EVT_PUBACK: id = 1 result = 0
    [00:00:19.141,265] <dbg> cloud: on_mqtt_puback: Publish acknowledgment received, message id: 1
    [00:00:19.141,296] <dbg> mqtt_helper: mqtt_helper_poll_loop: Polling on socket fd: 0
    [00:00:48.653,503] <dbg> mqtt_helper: mqtt_helper_poll_loop: Polling on socket fd: 0
    [00:00:49.587,463] <dbg> mqtt_helper: mqtt_evt_handler: MQTT_EVT_PINGRESP
    [00:00:49.587,493] <dbg> mqtt_helper: mqtt_helper_poll_loop: Polling on socket fd: 0
    [00:01:18.697,692] <dbg> mqtt_helper: mqtt_helper_poll_loop: Polling on socket fd: 0
    [00:01:19.350,921] <dbg> mqtt_helper: mqtt_evt_handler: MQTT_EVT_PINGRESP
    ```

### **Module State Machine**

The cloud MQTT module implements an internal state machine to manage the connection and reconnection logic.

   ```mermaid
   stateDiagram-v2
       [*] --> STATE_RUNNING
       STATE_RUNNING --> STATE_DISCONNECTED : NETWORK_DISCONNECTED
       STATE_DISCONNECTED --> STATE_CONNECTING : NETWORK_CONNECTED
       STATE_CONNECTING --> STATE_CONNECTING_ATTEMPT
       STATE_CONNECTING_ATTEMPT --> STATE_CONNECTING_BACKOFF : CLOUD_CONN_FAILED
       STATE_CONNECTING_BACKOFF --> STATE_CONNECTING_ATTEMPT : CLOUD_BACKOFF_EXPIRED
       STATE_CONNECTING_ATTEMPT --> STATE_CONNECTED : CLOUD_CONN_SUCCESS
       STATE_CONNECTED --> STATE_DISCONNECTED : NETWORK_DISCONNECTED
       STATE_CONNECTED --> STATE_CONNECTED : PAYLOAD_CHAN / send_data()
       STATE_CONNECTED --> STATE_CONNECTED : <module>_SAMPLE_RESPONSE / send_<module>_data()
       STATE_CONNECTED --> STATE_CONNECTED : NETWORK_CONNECTED / (noop)
       STATE_CONNECTED --> STATE_DISCONNECTED : exit / mqtt_helper_disconnect()
   ```

### Limitations

The MQTT cloud module is designed as a demonstration of how to replace the template's default nRF Cloud CoAP-based cloud module with an MQTT-based implementation. It is not intended to be a fully-featured solution and has the following limitations:

- **Location and FOTA Support**:
  The example MQTT module does not support location services or firmware over-the-air (FOTA) updates, as these features rely on nRF Cloud CoAP functionality.

- **Stub Channels for FOTA and LOCATION**:
  To prevent build errors, the MQTT module includes placeholder (stub) channel declarations for FOTA and LOCATION. If your application requires these features, you will need to implement custom modules tailored to your chosen cloud service.

For production use, it is recommended to utilize the default nRF Cloud CoAP cloud module, which provides comprehensive support for location services, FOTA, and other advanced features.
