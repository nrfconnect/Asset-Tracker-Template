# Lesson 4: Working with Core Modules

## Lesson Overview

In this lesson, you will explore the **core modules** of the Asset Tracker Template in detail. You'll understand what each module does, how they interact with each other, their configuration options, and how to customize their behavior for your specific application needs.

### Learning Objectives

By the end of this lesson, you will be able to:

- Understand the role and responsibilities of each core module
- Explain the interactions between modules
- Configure modules using Kconfig options
- Customize module behavior for specific use cases
- Debug module operations using logs and shell commands
- Modify business logic in the main module

### Duration

Approximately 120-150 minutes

## 1. Module Overview

### 1.1 Module Categories

The Asset Tracker Template modules can be categorized by their primary function:

| Category | Modules | Description |
|----------|---------|-------------|
| **Coordination** | Main | Central business logic coordinator |
| **Connectivity** | Network, Cloud | LTE and cloud communication |
| **Data Collection** | Location, Environmental, Power | Sensor and positioning data |
| **Data Management** | Storage | Data buffering and persistence |
| **System Services** | FOTA | Firmware updates |
| **User Interface** | LED, Button | Visual feedback and user input |

### 1.2 Module Dependencies

```
┌──────────┐
│   MAIN   │ ◄─── Central coordinator
└─┬──┬──┬──┘
  │  │  │
  │  │  └─────────┐
  │  │            │
  │  │   ┌────────▼──────┐
  │  │   │   LOCATION    │
  │  │   └───────────────┘
  │  │
  │  │   ┌───────────────┐
  │  └───►  ENVIRONMENTAL│
  │      └───────────────┘
  │
  │      ┌───────────────┐
  └──────►    NETWORK    │
         └───────┬───────┘
                 │
         ┌───────▼───────┐
         │     CLOUD     │ ◄─── Depends on network
         └───────┬───────┘
                 │
         ┌───────▼───────┐
         │    STORAGE    │ ◄─── Receives data from all sensors
         └───────────────┘
                 │
         ┌───────▼───────┐
         │     FOTA      │ ◄─── Depends on cloud
         └───────────────┘

┌────────┐  ┌────────┐
│  LED   │  │ BUTTON │ ◄─── User interface
└────────┘  └────────┘

┌────────┐
│ POWER  │ ◄─── System monitoring
└────────┘
```

## 2. Main Module

### 2.1 Purpose

The **Main module** is the **central coordinator** that implements your application's business logic. It:

- Coordinates all other modules
- Implements sampling schedules and triggers
- Manages the application state machine
- Handles user interactions (button presses)
- Controls LED visual feedback
- Processes configuration updates from cloud

**Key insight:** Most application customization happens in the main module.

### 2.2 State Machine

The main module has a hierarchical state machine:

```
STATE_RUNNING (parent)
├── STATE_INIT
│   └─── Initialize system, wait for cloud connection
│
├── STATE_OPERATIONAL (parent)
│   ├── STATE_SAMPLE_ENVIRONMENTAL
│   │   └─── Request environmental sensor data
│   ├── STATE_SAMPLE_LOCATION
│   │   └─── Request location data
│   └── STATE_SAMPLE_NETWORK_QUALITY
│       └─── Request network metrics
│
└── STATE_FOTA_UPDATING
    └─── Handle firmware update process
```

### 2.3 Typical Operational Flow

```
1. INIT STATE
   ├─> Wait for network connection
   ├─> Wait for cloud connection
   └─> Transition to OPERATIONAL

2. OPERATIONAL STATE (repeating cycle)
   ├─> Start periodic timer
   ├─> Request location (STATE_SAMPLE_LOCATION)
   │   └─> Wait for LOCATION_SEARCH_DONE
   ├─> Request environmental data (STATE_SAMPLE_ENVIRONMENTAL)
   │   └─> Wait for ENVIRONMENTAL_SAMPLE_RESPONSE
   ├─> Request network quality (STATE_SAMPLE_NETWORK_QUALITY)
   │   └─> Wait for NETWORK_QUALITY_RESPONSE
   ├─> Data automatically sent to cloud by storage/cloud modules
   └─> Return to idle, wait for next trigger

3. BUTTON PRESS
   ├─> Interrupt idle
   └─> Trigger immediate sampling cycle

4. FOTA UPDATE AVAILABLE
   ├─> Enter STATE_FOTA_UPDATING
   ├─> Download and apply update
   └─> Reboot
```

### 2.4 Key Configuration Options

```kconfig
# Sampling intervals
CONFIG_APP_SENSOR_SAMPLING_INTERVAL_SECONDS=300
    # How often to sample sensors and location (default: 5 minutes)

CONFIG_APP_CLOUD_SYNC_INTERVAL_SECONDS=600
    # How often to sync with cloud (default: 10 minutes)

# Features
CONFIG_APP_REQUEST_NETWORK_QUALITY=y
    # Include network quality metrics in samples

# Thread configuration
CONFIG_APP_WATCHDOG_TIMEOUT_SECONDS=60
CONFIG_APP_MSG_PROCESSING_TIMEOUT_SECONDS=30
```

### 2.5 Customizing Business Logic

The main module is where you customize application behavior:

**Example: Change LED color based on temperature**

```c
static enum smf_state_result state_sample_environmental_run(void *obj)
{
    struct main_state *state = (struct main_state *)obj;
    
    if (state->chan == &ENVIRONMENTAL_CHAN) {
        struct environmental_msg msg = MSG_TO_ENVIRONMENTAL_MSG(state->msg_buf);
        
        if (msg.type == ENVIRONMENTAL_SAMPLE_RESPONSE) {
            /* Custom logic: Change LED based on temperature */
            struct led_msg led = {
                .type = LED_RGB_SET,
                .duration_on_msec = 1000,
                .duration_off_msec = 0,
                .repetitions = 1,
            };
            
            if (msg.temperature > 25.0) {
                /* Hot - red LED */
                led.red = 255;
                led.green = 0;
                led.blue = 0;
            } else if (msg.temperature < 15.0) {
                /* Cold - blue LED */
                led.red = 0;
                led.green = 0;
                led.blue = 255;
            } else {
                /* Normal - green LED */
                led.red = 0;
                led.green = 255;
                led.blue = 0;
            }
            
            zbus_chan_pub(&LED_CHAN, &led, K_SECONDS(1));
            
            /* Continue with normal processing */
            smf_set_state(SMF_CTX(state), &states[STATE_OPERATIONAL]);
            return SMF_EVENT_CONSUMED;
        }
    }
    
    return SMF_EVENT_PROPAGATE;
}
```

## 3. Network Module

### 3.1 Purpose

The **Network module** manages LTE connectivity. It:

- Initializes the modem
- Connects to LTE network (LTE-M or NB-IoT)
- Monitors connection status
- Handles network events
- Provides network quality metrics
- Manages power-saving features (PSM, eDRX)

### 3.2 State Machine

```
STATE_RUNNING
├── STATE_DISCONNECTED
│   ├── STATE_DISCONNECTED_IDLE
│   │   └─── Not searching for network
│   └── STATE_DISCONNECTED_SEARCHING
│       └─── Actively searching for LTE network
├── STATE_CONNECTED
│   └─── Connected to LTE network
└── STATE_DISCONNECTING
    └─── Disconnecting from network
```

### 3.3 Message Flow

**Network connection sequence:**

```
Network Module                Main Module                 Cloud Module
      │                           │                           │
      │ NETWORK_CONNECTED ──────> │                           │
      │                           │                           │
      │                           │ <─────────────────────────┤
      │                           │      (observing)          │
      │                           │                           │
      │                           │        NETWORK_CONNECTED ─┤
      │                           │                           │
      │                           │                        Connect to
      │                           │                        nRF Cloud
```

### 3.4 Key Configuration Options

```kconfig
# Network mode
CONFIG_LTE_NETWORK_MODE_LTE_M=y
    # Use LTE-M (faster, higher throughput)
# CONFIG_LTE_NETWORK_MODE_NBIOT=y
    # Use NB-IoT (better coverage, lower power)

# Power saving
CONFIG_LTE_PSM_REQ=y
    # Enable Power Saving Mode
CONFIG_LTE_PSM_REQ_RPTAU="00000110"
    # Periodic TAU timer (6 hours)
CONFIG_LTE_PSM_REQ_RAT="00000000"
    # Active time (0 seconds - deep sleep immediately)

# eDRX (Extended Discontinuous Reception)
CONFIG_LTE_EDRX_REQ=y
CONFIG_LTE_EDRX_REQ_VALUE_LTE_M="0010"
    # eDRX cycle length

# Network search
CONFIG_LTE_NETWORK_TIMEOUT=600
    # Connection timeout (seconds)
```

### 3.5 Network Quality Metrics

The network module can provide quality metrics:

```c
struct network_msg msg = {
    .type = NETWORK_QUALITY_RESPONSE,
    .rsrp = -95,              /* Reference Signal Received Power (dBm) */
    .cell_id = 12345678,      /* Cell ID */
    .area_code = 1234,        /* Tracking Area Code */
};
```

**RSRP interpretation:**
- **-80 dBm or better**: Excellent signal
- **-90 dBm**: Good signal
- **-100 dBm**: Fair signal
- **-110 dBm or worse**: Poor signal

### 3.6 Debugging Network Issues

Enable detailed modem logging:

```kconfig
CONFIG_MODEM_INFO_LOG_LEVEL_DBG=y
CONFIG_LTE_LINK_CONTROL_LOG_LEVEL_DBG=y
```

Check network status via shell:

```bash
uart:~$ kernel threads
# Look for "network" thread

uart:~$ log
# View connection attempts and errors
```

## 4. Location Module

### 4.1 Purpose

The **Location module** provides positioning services using:

- **GNSS** (GPS, GLONASS, Galileo, BeiDou)
- **Wi-Fi positioning** (using nRF Cloud)
- **Cellular positioning** (using nRF Cloud)

It uses the nRF Connect SDK's [Location library](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/libraries/modem/location.html) which handles method selection and fallback automatically.

### 4.2 Location Methods

| Method | Accuracy | Time to Fix | Power | Requirements |
|--------|----------|-------------|-------|--------------|
| **GNSS** | 5-10 m | 30-60 sec | High | Clear sky view |
| **Wi-Fi** | 20-50 m | 2-5 sec | Low | Wi-Fi APs nearby |
| **Cellular** | 100-1000 m | 1-2 sec | Very Low | LTE connected |

**Method priority:**

The Location library tries methods in order:
1. GNSS (if configured and enabled)
2. Wi-Fi (if available and enabled)
3. Cellular (fallback)

### 4.3 Message Flow

**Location request sequence:**

```
Main Module          Location Module          Cloud Module
      │                    │                       │
      │ LOCATION_REQUEST ─>│                       │
      │                    │                       │
      │                    │ Start GNSS            │
      │                    │                       │
      │<─ LOCATION_SEARCH_STARTED                  │
      │                    │                       │
      │                    │ (30-60 seconds)       │
      │                    │                       │
      │                    │ AGNSS needed?         │
      │                    ├─ LOCATION_AGNSS_REQ ─>│
      │                    │                       │
      │                    │<─ A-GNSS data ────────┤
      │                    │                       │
      │                    │ Fix acquired          │
      │<─ LOCATION_SEARCH_DONE                     │
      │   (lat, lon, accuracy)                     │
```

### 4.4 Key Configuration Options

```kconfig
# Enable location module
CONFIG_APP_LOCATION=y

# GNSS configuration
CONFIG_LOCATION_METHOD_GNSS=y
CONFIG_LOCATION_METHOD_GNSS_TIMEOUT=60000
    # GNSS timeout (milliseconds)

# Wi-Fi positioning
CONFIG_LOCATION_METHOD_WIFI=y
CONFIG_LOCATION_METHOD_WIFI_SCANNING_TIMEOUT=10000

# Cellular positioning
CONFIG_LOCATION_METHOD_CELLULAR=y

# A-GNSS (Assisted GNSS)
CONFIG_NRF_CLOUD_AGNSS=y
    # Use nRF Cloud for A-GNSS data
```

### 4.5 Location Data

```c
struct location_msg {
    enum location_msg_type type;
    
    /* Location data */
    double latitude;          /* Degrees */
    double longitude;         /* Degrees */
    double accuracy;          /* Meters */
    
    /* Method used */
    enum location_method {
        LOCATION_METHOD_GNSS,
        LOCATION_METHOD_WIFI,
        LOCATION_METHOD_CELLULAR,
    } method;
    
    /* Timestamp */
    int64_t timestamp_ms;
};
```

### 4.6 Improving GNSS Performance

**Tips for better GNSS fixes:**

1. **Use A-GNSS** - Dramatically reduces time to first fix
   ```kconfig
   CONFIG_NRF_CLOUD_AGNSS=y
   ```

2. **Increase timeout** - Give GNSS more time in challenging conditions
   ```kconfig
   CONFIG_LOCATION_METHOD_GNSS_TIMEOUT=120000  # 2 minutes
   ```

3. **Position antenna properly** - Ensure clear sky view

4. **Consider fallback** - Use Wi-Fi/cellular when GNSS fails
   ```kconfig
   CONFIG_LOCATION_METHOD_WIFI=y
   ```

## 5. Cloud Module

### 5.1 Purpose

The **Cloud module** handles communication with **nRF Cloud** using the **CoAP protocol**. It:

- Establishes secure connection to nRF Cloud
- Handles device provisioning
- Sends sensor data and location updates
- Receives configuration updates (device shadow)
- Manages connection lifecycle and retries
- Provides A-GNSS data for location module

### 5.2 State Machine

```
STATE_RUNNING
├── STATE_DISCONNECTED
│   └─── Not connected to cloud
├── STATE_CONNECTING
│   ├── STATE_CONNECTING_ATTEMPT
│   │   └─── Attempting connection
│   └── STATE_CONNECTING_BACKOFF
│       └─── Waiting before retry
├── STATE_CONNECTED
│   ├── STATE_CONNECTED_READY
│   │   └─── Connected, ready to send data
│   └── STATE_CONNECTED_PROVISIONING
│       └─── Provisioning credentials
└── STATE_DISCONNECTING
    └─── Disconnecting from cloud
```

### 5.3 CoAP Protocol

**CoAP (Constrained Application Protocol)** is optimized for IoT:

- **UDP-based** - Lower overhead than TCP
- **Small packets** - Efficient for cellular
- **DTLS security** - Encrypted communication
- **Confirmable messages** - Optional reliability

**Why CoAP vs MQTT:**
- ✅ Lower power consumption
- ✅ Smaller protocol overhead
- ✅ Better for intermittent connectivity
- ✅ Native to nRF Cloud

### 5.4 Data Transmission

Data is sent to nRF Cloud via the storage module:

```
Environmental Module ─┐
Location Module      ─┼─> Storage Module ─> Cloud Module ─> nRF Cloud
Power Module         ─┘
```

The cloud module receives data from storage and sends it using nRF Cloud APIs:

```c
/* Send location data */
err = nrf_cloud_coap_location_send(
    &location,
    confirmable,
    &timestamp_ms
);

/* Send sensor data */
err = nrf_cloud_coap_sensor_send(
    "TEMP",
    temperature_value,
    timestamp_ms
);
```

### 5.5 Configuration Updates

The cloud module receives configuration from nRF Cloud **device shadow**:

```json
{
  "state": {
    "desired": {
      "config": {
        "sampleIntervalSeconds": 600,
        "locationTimeout": 120,
        "activeMode": false
      }
    }
  }
}
```

The module applies these updates to change runtime behavior.

### 5.6 Key Configuration Options

```kconfig
# Cloud module
CONFIG_APP_CLOUD=y

# nRF Cloud CoAP
CONFIG_NRF_CLOUD_COAP=y
CONFIG_NRF_CLOUD_COAP_SEC_TAG=16842753
    # Security tag for credentials

# Connection behavior
CONFIG_APP_CLOUD_BACKOFF_INITIAL_SECONDS=10
CONFIG_APP_CLOUD_BACKOFF_MAX_SECONDS=3600
CONFIG_APP_CLOUD_BACKOFF_TYPE_EXPONENTIAL=y

# Features
CONFIG_NRF_CLOUD_AGNSS=y
    # A-GNSS support
CONFIG_NRF_CLOUD_PGPS=y
    # P-GPS (Predicted GPS) support
```

## 6. Environmental Module

### 6.1 Purpose

The **Environmental module** collects sensor data from:

- **Temperature** sensor
- **Humidity** sensor
- **Pressure** sensor
- **(Optional) Additional sensors** (can be added)

On Thingy:91 X, it uses the **BME680** environmental sensor.

### 6.2 Message Flow

```
Main Module       Environmental Module       BME680 Sensor
     │                    │                        │
     │ ENV_REQUEST ──────>│                        │
     │                    │                        │
     │                    │ sensor_sample_fetch() ─>│
     │                    │                        │
     │                    │<── Sample data ────────┤
     │                    │                        │
     │                    │ sensor_channel_get()   │
     │                    │                        │
     │<── ENV_RESPONSE ───┤                        │
     │ (temp, humidity, pressure)                  │
```

### 6.3 Sensor Data

```c
struct environmental_msg {
    enum environmental_msg_type type;
    
    double temperature;    /* Celsius */
    double humidity;       /* Percent (0-100) */
    double pressure;       /* kPa */
    
    int64_t timestamp_ms;
};
```

### 6.4 Adding Sensors

To add a new sensor (e.g., magnetometer), see Lesson 6 or the [customization guide](../docs/common/customization.md#add-environmental-sensor).

### 6.5 Key Configuration Options

```kconfig
# Enable module
CONFIG_APP_ENVIRONMENTAL=y

# Sensor drivers
CONFIG_SENSOR=y
CONFIG_BME680=y          # Environmental sensor

# Sampling
CONFIG_APP_ENVIRONMENTAL_SAMPLE_INTERVAL_SECONDS=300
```

## 7. Storage Module

### 7.1 Purpose

The **Storage module** manages data collection and persistence. It:

- Receives data from all sensor modules
- Buffers data when cloud is unavailable
- Persists data to flash memory
- Forwards data to cloud when connected
- Manages storage capacity

### 7.2 Operating Modes

The storage module has two modes:

| Mode | Behavior | Use Case |
|------|----------|----------|
| **Passthrough** | Send data immediately to cloud | Good connectivity, real-time data |
| **Buffer** | Store locally, batch send later | Poor connectivity, offline operation |

Mode is selected automatically based on cloud connection status.

### 7.3 Data Types

The storage module handles multiple data types:

```c
enum storage_data_type {
    STORAGE_TYPE_LOCATION,
    STORAGE_TYPE_ENVIRONMENTAL,
    STORAGE_TYPE_POWER,
    STORAGE_TYPE_NETWORK_QUALITY,
};
```

Each type has its own encoding and handling.

### 7.4 Data Flow

**Passthrough mode (cloud connected):**
```
Sensor Module ─> Storage Module ─> Cloud Module ─> nRF Cloud
                 (immediate)
```

**Buffer mode (cloud disconnected):**
```
Sensor Module ─> Storage Module ─> Flash Memory
                 (buffered)

Later, when connected:
Flash Memory ─> Storage Module ─> Cloud Module ─> nRF Cloud
```

### 7.5 Key Configuration Options

```kconfig
# Enable storage
CONFIG_APP_STORAGE=y

# Flash storage
CONFIG_APP_STORAGE_FLASH=y
CONFIG_APP_STORAGE_FLASH_SIZE=16384
    # Storage size in bytes

# Buffering
CONFIG_APP_STORAGE_MAX_ITEMS=50
    # Maximum items to buffer
```

## 8. Other Modules

### 8.1 FOTA Module

Manages **Firmware Over-The-Air** updates:

- Polls nRF Cloud for available updates
- Downloads firmware images
- Applies updates using MCUboot
- Handles rollback on failure

```kconfig
CONFIG_APP_FOTA=y
CONFIG_NRF_CLOUD_FOTA=y
```

### 8.2 Power Module

Monitors battery and power status:

- Battery voltage and percentage
- Charging status (VBUS connected)
- Power management events

```c
struct power_msg {
    enum power_msg_type type;
    
    int battery_voltage;      /* Millivolts */
    int battery_percentage;   /* 0-100 */
    bool vbus_connected;      /* Charging */
};
```

### 8.3 LED Module

Controls RGB LED for visual feedback:

```c
struct led_msg {
    enum led_msg_type type;
    
    uint8_t red;              /* 0-255 */
    uint8_t green;            /* 0-255 */
    uint8_t blue;             /* 0-255 */
    uint16_t duration_on_msec;
    uint16_t duration_off_msec;
    uint8_t repetitions;
};
```

**Default LED patterns:**
- **Yellow pulsing** - Acquiring location
- **Green flash** - Sample complete
- **Blue** - Cloud connected
- **Red** - Error condition

### 8.4 Button Module

Handles button press events:

```c
struct button_msg {
    enum button_msg_type type;
    uint8_t button_id;
    bool pressed;
};
```

## 9. Module Interactions Example

### 9.1 Complete Sampling Cycle

Here's a complete trace of a typical sampling cycle:

```
1. TIMER EXPIRES
   Main: Timer triggers sampling

2. REQUEST LOCATION
   Main ──LOCATION_REQUEST──> Location
   
3. LOCATION SEARCH
   Location: Start GNSS
   Location ──LOCATION_SEARCH_STARTED──> Main
   Main ──LED_SET(yellow, pulsing)──> LED
   
4. A-GNSS REQUEST (if needed)
   Location ──LOCATION_AGNSS_REQUEST──> Cloud
   Cloud: Fetch A-GNSS from nRF Cloud
   Cloud ──AGNSS_DATA──> Location
   
5. LOCATION FOUND
   Location: GNSS fix acquired
   Location ──LOCATION_SEARCH_DONE──> Main, Storage
   Storage: Buffer location data
   
6. REQUEST ENVIRONMENTAL DATA
   Main ──ENVIRONMENTAL_REQUEST──> Environmental
   Environmental: Sample BME680
   Environmental ──ENVIRONMENTAL_RESPONSE──> Main, Storage
   Storage: Buffer sensor data
   
7. REQUEST POWER STATUS
   Main ──POWER_REQUEST──> Power
   Power: Read battery
   Power ──POWER_RESPONSE──> Main, Storage
   Storage: Buffer power data
   
8. SEND TO CLOUD
   Storage ──STORAGE_DATA──> Cloud
   Cloud: Send all data via CoAP
   Cloud ──CLOUD_DATA_SENT──> Main
   
9. UPDATE LED
   Main ──LED_SET(green, flash)──> LED
   
10. RETURN TO IDLE
    Main: Wait for next trigger
```

## Summary

In this lesson, you learned:

✅ The main module coordinates all other modules and implements business logic
✅ The network module manages LTE connectivity with power optimization
✅ The location module provides GNSS, Wi-Fi, and cellular positioning
✅ The cloud module handles nRF Cloud communication via CoAP
✅ The storage module buffers data and manages persistence
✅ Other modules provide sensor data, UI, and system services
✅ Modules interact through zbus messages in well-defined patterns
✅ Each module can be configured via Kconfig options

## Exercises

### Exercise 1: Configure Module Behavior

Modify module configurations to change application behavior (sampling intervals, network mode, location methods).

See [Exercise 1 instructions](../exercises/lesson4/exercise1/README.md)

### Exercise 2: Modify Main Module Business Logic

Customize the main module to implement custom sampling logic based on sensor readings.

See [Exercise 2 instructions](../exercises/lesson4/exercise2/README.md)

## Next Steps

In the next lesson, you'll learn about **Cloud Connectivity and Data Management** - understanding CoAP communication, CBOR encoding, device shadows, and FOTA updates in detail.

Continue to [Lesson 5: Cloud Connectivity and Data Management](lesson5_cloud_connectivity.md)

