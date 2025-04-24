# Customization

## Table of Contents

- [Add environmental sensor](#add-environmental-sensor)
- [Add your own module](#add-your-own-module)

This section contains guides for modifying specific aspects of the template.

## Add environmental sensor

TL;DR: To add basic support for the BMM350 magnetometer, apply the following patch:

```bash
git apply <path-to-template-dir>/Asset-Tracker-Template/docs/patches/magnetometer.patch
```

To add a new sensor to the environmental module, ensure that:

1. The sensor is properly configured in the Device Tree (DTS)
2. The corresponding driver is available in the Zephyr RTOS
3. The sensor is compatible with the Zephyr Sensor API

In this example, we'll add support for the Bosch BMM350 Magnetometer sensor using the [Zephyr Sensor API](https://docs.zephyrproject.org/latest/hardware/peripherals/sensor/index.html). The BMM350 driver in Zephyr integrates with the Sensor API, making it straightforward to use.
This guide will apply in general for all sensors that support the Zephyr Sensor API.

This guide uses the Thingy91x as an example, as it's a supported board in the template with defined board files in the nRF Connect SDK (NCS).

1. First, enable the sensor in the Device Tree by setting its status to "okay". This will:
   - Instantiate the Device Tree node for the sensor
   - Initialize the driver during boot
   - Make the sensor ready for use

Add the following to the board-specific Device Tree overlay file (`thingy91x_nrf9151_ns.overlay`):

```c
&magnetometer {
    status = "okay";
};
```

2. Update the environmental module's state structure to include the magnetometer device reference and data storage:

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

3. Initialize the device reference using the Device Tree label:

```c
struct environmental_state environmental_state = {
    .bme680 = DEVICE_DT_GET(DT_NODELABEL(bme680)),
    .bmm350 = DEVICE_DT_GET(DT_NODELABEL(magnetometer)),
};
```

4. Update the sensor sampling function signature to include the magnetometer device:

```c
static void sample_sensors(const struct device *const bme680, const struct device *const bmm350)
```

And update the function call:

```c
sample_sensors(state_object->bme680, state_object->bmm350);
```

5. Implement sensor data acquisition using the Zephyr Sensor API:

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

6. Add cloud integration for the magnetometer data:

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

Define a custom APP ID for magnetic field data:

```c
#define CUSTOM_JSON_APPID_VAL_MAGNETIC "MAGNETIC_FIELD"
```

## Add your own module

TL;DR: To add the dummy module to the template, apply the following patch:

```bash
git apply <path-to-template-dir>/Asset-Tracker-Template/docs/patches/dummy-module.patch
```

This guide demonstrates how to create a new module in the template. The dummy module serves as a template for understanding the module architecture and can be used as a foundation for custom modules.

1. Create the module directory structure:

```bash
mkdir -p app/src/modules/dummy
```

2. Create the following files in the module directory:
   - `dummy.h` - Module interface definitions
   - `dummy.c` - Module implementation
   - `Kconfig.dummy` - Module configuration options
   - `CMakeLists.txt` - Build system configuration

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

4. In `dummy.c`, implement the module's functionality:

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

5. In `Kconfig.dummy`, define module configuration options:

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

6. In `CMakeLists.txt`, configure the build system:

```cmake
target_sources_ifdef(CONFIG_APP_DUMMY app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/dummy.c)
target_include_directories(app PRIVATE .)
```

7. Add the module to the main application's CMakeLists.txt:

```cmake
add_subdirectory(src/modules/dummy)
```

The dummy module is now ready to use. It provides the following functionality:

- Initializes with a counter value of 0
- Increments the counter on each sample request
- Responds with the current counter value using zbus
- Includes error handling and watchdog support
- Follows the state machine pattern used by other modules

To test the module, send a `DUMMY_SAMPLE_REQUEST` message to its zbus channel. The module will respond with a `DUMMY_SAMPLE_RESPONSE` containing the incremented counter value.

This dummy module serves as a template that you can extend to implement more complex functionality. You can add additional message types, state variables, and processing logic as needed for your specific use case.
