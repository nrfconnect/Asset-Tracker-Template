# Lesson 6: Customization and Adding New Features

## Lesson Overview

In this final lesson, you will learn how to customize the Asset Tracker Template for your specific use case. You'll add new sensors, create custom modules, implement power optimization techniques, and explore alternative cloud backends. This lesson brings together all concepts from previous lessons.

### Learning Objectives

By the end of this lesson, you will be able to:

- Add new environmental sensors to the template
- Create custom modules from the dummy template
- Extend existing modules with new functionality
- Implement power optimization strategies
- Configure alternative cloud backends (MQTT)
- Plan and execute custom IoT applications
- Apply best practices for production deployment

### Duration

Approximately 120-180 minutes

## 1. Adding Environmental Sensors

### 1.1 Overview

Adding a new sensor involves:

1. **Device Tree** configuration (hardware definition)
2. **Module** extension (software integration)
3. **Data flow** setup (message handling)
4. **Cloud integration** (data transmission)

### 1.2 Example: Adding BMM350 Magnetometer

The BMM350 is a 3-axis magnetometer available on some Nordic boards.

**Step 1: Enable in Device Tree**

Edit `boards/thingy91x_nrf9151_ns.overlay`:

```dts
&magnetometer {
    status = "okay";
};
```

This tells Zephyr to:
- Initialize the BMM350 driver
- Make the device available via Device API
- Configure I2C communication

**Step 2: Extend Environmental Module**

Edit `src/modules/environmental/environmental.c`:

Add device reference to state:
```c
struct environmental_state_object {
    /* Existing fields */
    const struct device *const bme680;
    
    /* New field */
    const struct device *const bmm350;
    
    /* Data storage */
    double magnetic_field[3];  /* X, Y, Z in µT */
};
```

Initialize device:
```c
struct environmental_state_object environmental_state = {
    .bme680 = DEVICE_DT_GET(DT_NODELABEL(bme680)),
    .bmm350 = DEVICE_DT_GET(DT_NODELABEL(magnetometer)),
};
```

**Step 3: Sample Sensor Data**

Update sampling function:

```c
static void sample_sensors(const struct device *bme680,
                           const struct device *bmm350,
                           struct environmental_state_object *state)
{
    int err;
    struct sensor_value magnetic[3];
    
    /* ... existing BME680 sampling ... */
    
    /* Sample BMM350 */
    err = sensor_sample_fetch(bmm350);
    if (err) {
        LOG_ERR("Failed to fetch magnetometer sample: %d", err);
        return;
    }
    
    err = sensor_channel_get(bmm350, SENSOR_CHAN_MAGN_XYZ, magnetic);
    if (err) {
        LOG_ERR("Failed to get magnetometer data: %d", err);
        return;
    }
    
    /* Store in state */
    state->magnetic_field[0] = sensor_value_to_double(&magnetic[0]);
    state->magnetic_field[1] = sensor_value_to_double(&magnetic[1]);
    state->magnetic_field[2] = sensor_value_to_double(&magnetic[2]);
    
    LOG_DBG("Magnetic field: X=%.2f, Y=%.2f, Z=%.2f µT",
            state->magnetic_field[0],
            state->magnetic_field[1],
            state->magnetic_field[2]);
}
```

**Step 4: Update Message Structure**

Edit `src/modules/environmental/environmental.h`:

```c
struct environmental_msg {
    enum environmental_msg_type type;
    
    /* Existing fields */
    double temperature;
    double humidity;
    double pressure;
    
    /* New field */
    double magnetic_field[3];  /* X, Y, Z in µT */
    
    int64_t timestamp_ms;
};
```

**Step 5: Integrate with Cloud**

Edit `src/modules/cloud/cloud.c`:

```c
static int send_environmental_data(const struct environmental_msg *env)
{
    int err;
    
    /* Send existing sensors */
    err = nrf_cloud_coap_sensor_send("TEMP", env->temperature, ...);
    err = nrf_cloud_coap_sensor_send("HUMID", env->humidity, ...);
    err = nrf_cloud_coap_sensor_send("PRESS", env->pressure, ...);
    
    /* Send magnetometer data */
    if (env->magnetic_field[0] != 0.0 ||
        env->magnetic_field[1] != 0.0 ||
        env->magnetic_field[2] != 0.0) {
        
        char message[64];
        snprintf(message, sizeof(message),
                "%.2f %.2f %.2f",
                env->magnetic_field[0],
                env->magnetic_field[1],
                env->magnetic_field[2]);
        
        err = nrf_cloud_coap_message_send(
            "MAGNETIC_FIELD",
            message,
            false,  /* Not binary */
            env->timestamp_ms,
            true    /* Confirmable */
        );
    }
    
    return 0;
}
```

**Step 6: Test**

Build and flash:
```bash
west build -p -b thingy91x/nrf9151/ns
west flash
```

Monitor logs:
```
[env] <dbg> Magnetic field: X=45.23, Y=-12.45, Z=32.10 µT
[cloud] <dbg> Magnetometer data sent to cloud
```

### 1.3 Sensor Integration Checklist

✅ Device Tree configuration
✅ Driver enabled in Kconfig
✅ Device reference in module state
✅ Device initialization
✅ Sampling logic implemented
✅ Message structure updated
✅ Cloud integration added
✅ Testing completed

## 2. Creating Custom Modules

### 2.1 When to Create a Module

Create a new module when you need:

- **Independent functionality** (e.g., custom sensor, actuator)
- **Blocking operations** (e.g., external communication)
- **Separate state machine** (e.g., complex behavior)
- **Reusable component** (e.g., can be used across projects)

### 2.2 Module Template Structure

The template provides a dummy module as a starting point:

```
modules/dummy/
├── dummy.h              # Public interface
├── dummy.c              # Implementation
├── Kconfig.dummy        # Configuration options
└── CMakeLists.txt       # Build configuration
```

### 2.3 Creating an Accelerometer Module

Let's create a module to handle the accelerometer and detect motion events.

**Step 1: Create Module Structure**

```bash
mkdir -p app/src/modules/accelerometer
cd app/src/modules/accelerometer
```

**Step 2: Define Interface (`accelerometer.h`)**

```c
#ifndef _ACCELEROMETER_H_
#define _ACCELEROMETER_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Zbus channel */
ZBUS_CHAN_DECLARE(ACCELEROMETER_CHAN);

/* Message types */
enum accelerometer_msg_type {
    /* Output messages */
    ACCELEROMETER_MOTION_DETECTED = 0x1,
    ACCELEROMETER_MOTION_STOPPED,
    ACCELEROMETER_SAMPLE_RESPONSE,
    
    /* Input messages */
    ACCELEROMETER_SAMPLE_REQUEST,
    ACCELEROMETER_ENABLE,
    ACCELEROMETER_DISABLE,
};

/* Message structure */
struct accelerometer_msg {
    enum accelerometer_msg_type type;
    
    /* Sample data */
    double accel_x;  /* m/s² */
    double accel_y;
    double accel_z;
    
    /* Motion detection */
    bool motion_active;
    
    int64_t timestamp_ms;
};

#define MSG_TO_ACCELEROMETER_MSG(_msg) \
    (*(const struct accelerometer_msg *)_msg)

#ifdef __cplusplus
}
#endif

#endif /* _ACCELEROMETER_H_ */
```

**Step 3: Implement Module (`accelerometer.c`)**

```c
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/smf.h>
#include <zephyr/drivers/sensor.h>

#include "app_common.h"
#include "accelerometer.h"

LOG_MODULE_REGISTER(accelerometer, CONFIG_APP_ACCELEROMETER_LOG_LEVEL);

/* Define zbus channel */
ZBUS_CHAN_DEFINE(ACCELEROMETER_CHAN,
                 struct accelerometer_msg,
                 NULL,
                 NULL,
                 ZBUS_OBSERVERS_EMPTY,
                 ZBUS_MSG_INIT(0)
);

/* Message subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(accelerometer_subscriber);
ZBUS_CHAN_ADD_OBS(ACCELEROMETER_CHAN, accelerometer_subscriber, 0);

/* State machine states */
enum accelerometer_state {
    STATE_RUNNING,
    STATE_ENABLED,
    STATE_DISABLED,
};

/* Module state */
struct accelerometer_state_obj {
    struct smf_ctx ctx;
    
    const struct zbus_channel *chan;
    uint8_t msg_buf[sizeof(struct accelerometer_msg)];
    
    const struct device *accel_dev;
    bool motion_detected;
    int64_t last_motion_time;
};

/* Forward declarations */
static enum smf_state_result state_enabled_run(void *o);
static enum smf_state_result state_disabled_run(void *o);

/* State machine definition */
static const struct smf_state states[] = {
    [STATE_RUNNING] = SMF_CREATE_STATE(
        NULL,
        NULL,
        NULL,
        NULL,
        &states[STATE_DISABLED]
    ),
    [STATE_ENABLED] = SMF_CREATE_STATE(
        NULL,
        state_enabled_run,
        NULL,
        &states[STATE_RUNNING],
        NULL
    ),
    [STATE_DISABLED] = SMF_CREATE_STATE(
        NULL,
        state_disabled_run,
        NULL,
        &states[STATE_RUNNING],
        NULL
    ),
};

/* Sample accelerometer */
static int sample_accelerometer(struct accelerometer_state_obj *state,
                               struct accelerometer_msg *msg)
{
    int err;
    struct sensor_value accel[3];
    
    err = sensor_sample_fetch(state->accel_dev);
    if (err) {
        LOG_ERR("Failed to fetch sample: %d", err);
        return err;
    }
    
    err = sensor_channel_get(state->accel_dev,
                            SENSOR_CHAN_ACCEL_XYZ,
                            accel);
    if (err) {
        LOG_ERR("Failed to get data: %d", err);
        return err;
    }
    
    msg->accel_x = sensor_value_to_double(&accel[0]);
    msg->accel_y = sensor_value_to_double(&accel[1]);
    msg->accel_z = sensor_value_to_double(&accel[2]);
    msg->timestamp_ms = k_uptime_get();
    
    /* Simple motion detection: check if magnitude exceeds threshold */
    double magnitude = sqrt(
        msg->accel_x * msg->accel_x +
        msg->accel_y * msg->accel_y +
        msg->accel_z * msg->accel_z
    );
    
    if (magnitude > CONFIG_APP_ACCELEROMETER_MOTION_THRESHOLD) {
        if (!state->motion_detected) {
            /* Motion started */
            state->motion_detected = true;
            msg->type = ACCELEROMETER_MOTION_DETECTED;
            msg->motion_active = true;
            LOG_INF("Motion detected");
        }
        state->last_motion_time = k_uptime_get();
    } else {
        /* Check if motion stopped */
        if (state->motion_detected &&
            (k_uptime_get() - state->last_motion_time) >
            CONFIG_APP_ACCELEROMETER_MOTION_TIMEOUT_MS) {
            state->motion_detected = false;
            msg->type = ACCELEROMETER_MOTION_STOPPED;
            msg->motion_active = false;
            LOG_INF("Motion stopped");
        }
    }
    
    return 0;
}

/* State: Enabled */
static enum smf_state_result state_enabled_run(void *o)
{
    struct accelerometer_state_obj *state = o;
    
    if (state->chan == &ACCELEROMETER_CHAN) {
        struct accelerometer_msg msg = MSG_TO_ACCELEROMETER_MSG(state->msg_buf);
        
        switch (msg.type) {
        case ACCELEROMETER_SAMPLE_REQUEST: {
            struct accelerometer_msg response = {
                .type = ACCELEROMETER_SAMPLE_RESPONSE
            };
            
            int err = sample_accelerometer(state, &response);
            if (err == 0) {
                zbus_chan_pub(&ACCELEROMETER_CHAN, &response, K_SECONDS(1));
            }
            return SMF_EVENT_CONSUMED;
        }
        
        case ACCELEROMETER_DISABLE:
            smf_set_state(SMF_CTX(state), &states[STATE_DISABLED]);
            return SMF_EVENT_CONSUMED;
            
        default:
            break;
        }
    }
    
    return SMF_EVENT_PROPAGATE;
}

/* State: Disabled */
static enum smf_state_result state_disabled_run(void *o)
{
    struct accelerometer_state_obj *state = o;
    
    if (state->chan == &ACCELEROMETER_CHAN) {
        struct accelerometer_msg msg = MSG_TO_ACCELEROMETER_MSG(state->msg_buf);
        
        if (msg.type == ACCELEROMETER_ENABLE) {
            smf_set_state(SMF_CTX(state), &states[STATE_ENABLED]);
            return SMF_EVENT_CONSUMED;
        }
    }
    
    return SMF_EVENT_PROPAGATE;
}

/* Module task */
static void accelerometer_task(void)
{
    struct accelerometer_state_obj state = {
        .accel_dev = DEVICE_DT_GET(DT_NODELABEL(accelerometer)),
        .motion_detected = false,
    };
    
    if (!device_is_ready(state.accel_dev)) {
        LOG_ERR("Accelerometer device not ready");
        return;
    }
    
    LOG_INF("Accelerometer module started");
    
    smf_set_initial(SMF_CTX(&state), &states[STATE_RUNNING]);
    
    while (true) {
        int err = zbus_sub_wait_msg(
            &accelerometer_subscriber,
            &state.chan,
            state.msg_buf,
            K_FOREVER
        );
        
        if (err == -ENOMSG) {
            continue;
        }
        
        smf_run_state(SMF_CTX(&state));
    }
}

K_THREAD_DEFINE(accelerometer_task_id,
                CONFIG_APP_ACCELEROMETER_THREAD_STACK_SIZE,
                accelerometer_task,
                NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
```

**Step 4: Configuration (`Kconfig.accelerometer`)**

```kconfig
menuconfig APP_ACCELEROMETER
    bool "Accelerometer module"
    default y
    select SENSOR
    help
        Enable the accelerometer module for motion detection.

if APP_ACCELEROMETER

config APP_ACCELEROMETER_THREAD_STACK_SIZE
    int "Thread stack size"
    default 2048

config APP_ACCELEROMETER_MOTION_THRESHOLD
    int "Motion detection threshold (m/s²)"
    default 5
    help
        Acceleration magnitude threshold for motion detection.

config APP_ACCELEROMETER_MOTION_TIMEOUT_MS
    int "Motion timeout (ms)"
    default 5000
    help
        Time after which motion is considered stopped.

module = APP_ACCELEROMETER
module-str = ACCELEROMETER
source "subsys/logging/Kconfig.template.log_config"

endif # APP_ACCELEROMETER
```

**Step 5: Build Configuration (`CMakeLists.txt`)**

```cmake
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/accelerometer.c)
target_include_directories(app PRIVATE .)
```

**Step 6: Add to Main CMakeLists.txt**

Edit `app/src/CMakeLists.txt`:

```cmake
add_subdirectory(modules/accelerometer)
```

**Step 7: Use in Main Module**

Edit `src/modules/main/main.c`:

```c
/* Add to channel list */
#define CHANNEL_LIST(X) \
    /* ... existing channels ... */ \
    X(ACCELEROMETER_CHAN, struct accelerometer_msg)

/* Handle messages */
if (state->chan == &ACCELEROMETER_CHAN) {
    struct accelerometer_msg msg =
        MSG_TO_ACCELEROMETER_MSG(state->msg_buf);
    
    if (msg.type == ACCELEROMETER_MOTION_DETECTED) {
        LOG_INF("Motion detected - trigger immediate sample");
        
        /* Trigger sampling */
        struct location_msg loc_msg = {
            .type = LOCATION_REQUEST
        };
        zbus_chan_pub(&LOCATION_CHAN, &loc_msg, K_SECONDS(1));
    }
}
```

### 2.4 Module Development Checklist

✅ Interface defined (header file)
✅ Implementation complete (source file)
✅ Kconfig options added
✅ CMakeLists.txt configured
✅ Integrated with other modules
✅ Tested independently
✅ Documentation updated

## 3. Power Optimization

### 3.1 Power Consumption Analysis

**Typical power consumption:**

| State | Current | Duration | Energy |
|-------|---------|----------|--------|
| **Active (GNSS)** | 80-150 mA | 30-60 sec | 0.67-2.5 mAh |
| **Active (LTE TX)** | 100-200 mA | 1-5 sec | 0.03-0.28 mAh |
| **Idle (RRC connected)** | 5-15 mA | Variable | - |
| **PSM (deep sleep)** | 5-15 µA | Most of time | Negligible |

**Example for hourly sampling:**
- Active: 60 sec/hour = 2 mAh
- PSM: 3540 sec/hour = 0.06 mAh
- **Total: ~2 mAh/hour = ~48 mAh/day**

With 1500 mAh battery: **~31 days**

### 3.2 LTE Power Saving Mode (PSM)

PSM allows the device to enter deep sleep between samples:

```kconfig
# Enable PSM
CONFIG_LTE_PSM_REQ=y

# Periodic TAU (T3412)
CONFIG_LTE_PSM_REQ_RPTAU="00000110"  # 6 hours

# Active Time (T3324)
CONFIG_LTE_PSM_REQ_RAT="00000000"    # 0 seconds (immediate sleep)
```

**PSM Timer Encoding:**

```
"00000110" = 6 hours

Bits 5-8: 0000 (multiplier = 10 minutes)
Bits 1-4: 0110 (value = 36)
Result: 36 × 10 minutes = 6 hours
```

**Common values:**
- `"00000001"` = 1 hour
- `"00000110"` = 6 hours
- `"00001000"` = 8 hours

### 3.3 eDRX (Extended Discontinuous Reception)

eDRX extends paging intervals for better power efficiency:

```kconfig
# Enable eDRX
CONFIG_LTE_EDRX_REQ=y

# eDRX cycle for LTE-M
CONFIG_LTE_EDRX_REQ_VALUE_LTE_M="0010"  # 20.48 seconds
```

**Trade-off:**
- Longer cycle = lower power, higher latency for incoming messages
- Shorter cycle = higher power, lower latency

### 3.4 Reducing GNSS Power

**Strategy 1: A-GNSS (Assisted GNSS)**

```kconfig
CONFIG_NRF_CLOUD_AGNSS=y
```

Reduces time-to-first-fix from 60s to 10-15s.

**Strategy 2: P-GPS (Predicted GPS)**

```kconfig
CONFIG_NRF_CLOUD_PGPS=y
CONFIG_NRF_CLOUD_PGPS_PREDICTION_PERIOD=240  # 10 days
```

Provides satellite predictions, enabling fixes without internet.

**Strategy 3: Location Method Priority**

Use cellular/Wi-Fi fallback when GNSS is slow:

```kconfig
CONFIG_LOCATION_METHOD_GNSS=y
CONFIG_LOCATION_METHOD_WIFI=y
CONFIG_LOCATION_METHOD_CELLULAR=y
```

### 3.5 Reducing Sampling Frequency

Adjust sampling intervals based on use case:

```kconfig
# Low frequency
CONFIG_APP_SENSOR_SAMPLING_INTERVAL_SECONDS=3600  # 1 hour

# Medium frequency
CONFIG_APP_SENSOR_SAMPLING_INTERVAL_SECONDS=300   # 5 minutes

# High frequency
CONFIG_APP_SENSOR_SAMPLING_INTERVAL_SECONDS=60    # 1 minute
```

### 3.6 Dynamic Power Management

Implement adaptive sampling based on conditions:

```c
/* In main module */
static int get_dynamic_sample_interval(void)
{
    /* Sample more frequently when moving */
    if (motion_detected) {
        return 60;  /* 1 minute */
    }
    
    /* Sample less frequently when stationary */
    if (battery_low) {
        return 3600;  /* 1 hour */
    }
    
    return 300;  /* 5 minutes (default) */
}
```

### 3.7 Power Optimization Checklist

✅ PSM enabled and configured
✅ eDRX tuned for use case
✅ A-GNSS enabled
✅ Sampling intervals optimized
✅ Unnecessary features disabled
✅ Dynamic power management implemented
✅ Power consumption measured

## 4. Alternative Cloud Backends (MQTT)

### 4.1 MQTT Cloud Module

The template includes an example MQTT cloud module in `examples/modules/cloud/`:

```
examples/modules/cloud/
├── cloud_mqtt.c         # MQTT implementation
├── cloud_mqtt_shell.c   # Shell commands
├── Kconfig.cloud_mqtt   # Configuration
├── overlay-mqtt.conf    # Overlay configuration
└── creds/              # Server certificates
```

### 4.2 Building with MQTT

```bash
cd app
west build -p -b thingy91x/nrf9151/ns -- \
    -DEXTRA_CONF_FILE="$(pwd)/../examples/modules/cloud/overlay-mqtt.conf"
```

### 4.3 MQTT Configuration

Key configuration options:

```kconfig
# Enable MQTT cloud module
CONFIG_APP_CLOUD_MQTT=y

# Broker settings
CONFIG_APP_CLOUD_MQTT_HOSTNAME="mqtt.nordicsemi.academy"
CONFIG_APP_CLOUD_MQTT_PORT=8883
CONFIG_APP_CLOUD_MQTT_SEC_TAG=12345

# Topics
CONFIG_APP_CLOUD_MQTT_PUB_TOPIC="device/{imei}/data"
CONFIG_APP_CLOUD_MQTT_SUB_TOPIC="device/{imei}/commands"

# Connection
CONFIG_APP_CLOUD_MQTT_BACKOFF_INITIAL_SECONDS=10
CONFIG_APP_CLOUD_MQTT_BACKOFF_MAX_SECONDS=3600
```

### 4.4 Using MQTT Cloud Module

The MQTT module follows the same interface as the CoAP module:

```c
/* Publish data */
int err = mqtt_cloud_publish(topic, payload, payload_len);

/* Subscribe to topics */
int err = mqtt_cloud_subscribe(topic);

/* Check connection status */
bool connected = mqtt_cloud_is_connected();
```

### 4.5 MQTT Module Limitations

⚠️ **Note:** The MQTT module is a demonstration example with limitations:

- No A-GNSS support (GNSS will be slower)
- No P-GPS support
- No cloud-based cellular positioning
- No integrated FOTA
- Custom data encoding required

For production, use **nRF Cloud CoAP** (default) for full feature support.

## 5. Best Practices for Production

### 5.1 Error Handling

✅ **Implement robust error handling:**

```c
/* Good: Handle all error cases */
int err = sensor_sample_fetch(dev);
if (err == -EBUSY) {
    LOG_WRN("Sensor busy, retry later");
    return -EAGAIN;
} else if (err == -EIO) {
    LOG_ERR("I/O error, sensor fault");
    SEND_FATAL_ERROR();
} else if (err) {
    LOG_ERR("Unexpected error: %d", err);
    return err;
}
```

❌ **Don't ignore errors:**

```c
/* Bad: Ignoring return values */
sensor_sample_fetch(dev);
sensor_channel_get(dev, SENSOR_CHAN_ALL, &val);
```

### 5.2 Watchdog Configuration

Configure appropriate watchdog timeouts:

```kconfig
# Main module
CONFIG_APP_WATCHDOG_TIMEOUT_SECONDS=60
CONFIG_APP_MSG_PROCESSING_TIMEOUT_SECONDS=30

# Network module
CONFIG_APP_NETWORK_WATCHDOG_TIMEOUT_SECONDS=120
CONFIG_APP_NETWORK_MSG_PROCESSING_TIMEOUT_SECONDS=90

# Location module (GNSS can take time)
CONFIG_APP_LOCATION_WATCHDOG_TIMEOUT_SECONDS=180
CONFIG_APP_LOCATION_MSG_PROCESSING_TIMEOUT_SECONDS=120
```

### 5.3 Testing

**Unit Testing:**

```bash
# Run unit tests
cd tests/module/<module_name>
west build -t run
```

**Integration Testing:**

```bash
# Run on-target tests
cd tests/on_target
python3 scripts/run_tests.py
```

**Field Testing:**

- Test in actual deployment environment
- Test with poor network conditions
- Test battery life over extended period
- Test FOTA updates end-to-end

### 5.4 Logging and Debugging

For production, adjust log levels:

```kconfig
# Reduce logging for production
CONFIG_LOG_DEFAULT_LEVEL=2  # Warning level
CONFIG_LOG_MODE_DEFERRED=y  # Asynchronous logging

# Keep important logs
CONFIG_APP_CLOUD_LOG_LEVEL_INF=y
CONFIG_APP_FOTA_LOG_LEVEL_INF=y
```

### 5.5 Security

✅ **Security checklist:**

- [ ] Attestation-based provisioning enabled
- [ ] Credentials stored in secure storage
- [ ] DTLS/TLS for all cloud communication
- [ ] Firmware signing enabled
- [ ] Shell disabled in production (or secured)
- [ ] Debug features disabled
- [ ] Modem security features enabled

```kconfig
# Production security
CONFIG_SECURE_BOOT=y
CONFIG_BOOTLOADER_MCUBOOT=y
CONFIG_MCUBOOT_SIGNATURE_KEY_FILE="production_key.pem"

# Disable debug features
CONFIG_SHELL=n
CONFIG_LOG_DEFAULT_LEVEL=2
```

## Summary

In this lesson, you learned:

✅ How to add new environmental sensors to the template
✅ Creating custom modules from the dummy template
✅ Extending existing modules with new functionality
✅ Implementing power optimization strategies (PSM, eDRX, A-GNSS)
✅ Using alternative cloud backends (MQTT)
✅ Best practices for production deployment
✅ Comprehensive testing and security considerations

## Exercises

### Exercise 1: Add a New Sensor

Add support for the BMM350 magnetometer sensor to the environmental module.

See [Exercise 1 instructions](../exercises/lesson6/exercise1/README.md)

### Exercise 2: Create a Custom Module

Create a custom module from the dummy template that implements specific functionality for your use case.

See [Exercise 2 instructions](../exercises/lesson6/exercise2/README.md)

## Course Completion

**Congratulations!** You have completed the Asset Tracker Template Developer Academy course.

You now have the skills to:
- ✅ Understand and work with the modular ATT architecture
- ✅ Use zbus for inter-module communication
- ✅ Implement state machines with SMF
- ✅ Configure and customize core modules
- ✅ Integrate with nRF Cloud via CoAP
- ✅ Add sensors and create custom modules
- ✅ Optimize power consumption
- ✅ Deploy production-ready IoT applications

### Next Steps

1. **Build your own application** - Apply what you've learned
2. **Explore advanced topics** - Deep dive into specific areas
3. **Join the community** - Share on [DevZone](https://devzone.nordicsemi.com/)
4. **Contribute** - Submit improvements to the ATT repository

### Additional Resources

- [Asset Tracker Template Documentation](https://docs.nordicsemi.com/bundle/asset-tracker-template-latest/)
- [nRF Connect SDK Documentation](https://docs.nordicsemi.com/bundle/ncs-latest/)
- [Nordic Developer Zone](https://devzone.nordicsemi.com/)
- [nRF Cloud Documentation](https://docs.nordicsemi.com/bundle/nrf-cloud/)

**Thank you for completing this course!**

