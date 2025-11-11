# Lesson 5: Cloud Connectivity and Data Management

## Lesson Overview

In this lesson, you will learn about cloud connectivity with nRF Cloud, understanding the CoAP protocol, CBOR encoding, device shadows, and firmware over-the-air (FOTA) updates. You'll learn how data flows from the device to the cloud and how to manage devices remotely.

### Learning Objectives

By the end of this lesson, you will be able to:

- Understand CoAP protocol and its benefits for IoT
- Explain CBOR encoding and how it reduces data usage
- Work with nRF Cloud device shadows for configuration
- Send custom data to nRF Cloud
- Perform firmware over-the-air (FOTA) updates
- Implement data buffering for offline operation
- Debug cloud connectivity issues

### Duration

Approximately 120-150 minutes

## 1. Introduction to nRF Cloud

### 1.1 What is nRF Cloud?

**nRF Cloud** is Nordic Semiconductor's IoT cloud platform that provides:

- **Device management** - Provisioning, monitoring, configuration
- **Location services** - A-GNSS, P-GPS, cellular/Wi-Fi positioning
- **Data collection** - Sensor data, telemetry, location tracking
- **Firmware updates** - Over-the-air (FOTA) updates
- **Analytics** - Data visualization and device insights

### 1.2 Why nRF Cloud?

Compared to building your own cloud infrastructure:

✅ **Faster development** - Pre-integrated with nRF Connect SDK
✅ **Location services** - Built-in A-GNSS and positioning
✅ **Security** - Attestation-based device provisioning
✅ **Scalability** - Production-ready infrastructure
✅ **Cost-effective** - Free tier available for development

### 1.3 nRF Cloud Architecture

```
┌─────────────┐
│   Device    │
│  (nRF9151)  │
└──────┬──────┘
       │ CoAP/DTLS
       │ over LTE
       ▼
┌──────────────┐
│  nRF Cloud   │
│   Gateway    │
└──────┬───────┘
       │
       ├──► Device Management
       ├──► Location Services (A-GNSS, P-GPS)
       ├──► Data Storage
       ├──► FOTA Management
       └──► REST API / Web UI
```

## 2. CoAP Protocol

### 2.1 What is CoAP?

**CoAP (Constrained Application Protocol)** is designed for resource-constrained IoT devices:

| Feature | CoAP | MQTT | HTTP |
|---------|------|------|------|
| **Transport** | UDP | TCP | TCP |
| **Overhead** | Very Low | Low | High |
| **Power Usage** | Lowest | Low | High |
| **Reliability** | Optional | Yes | Yes |
| **Security** | DTLS | TLS | TLS |
| **Best For** | Battery-powered | Always-connected | Web apps |

### 2.2 Why CoAP for Cellular IoT?

**Benefits for cellular IoT:**

✅ **Lower power consumption** - UDP requires less overhead than TCP
✅ **Smaller packets** - Less data usage on cellular
✅ **Faster connections** - No TCP handshake
✅ **Better for intermittent connectivity** - Handles connection drops gracefully
✅ **Native CoAP support in nRF Cloud** - Optimized for Nordic devices

**Example data savings:**

| Operation | HTTP + JSON | CoAP + CBOR | Savings |
|-----------|-------------|-------------|---------|
| Location request | ~1.2 kB | ~0.3 kB | **75%** |
| Sensor data | ~800 B | ~200 B | **75%** |

### 2.3 CoAP Message Types

CoAP uses four message types:

```c
/* Confirmable message - requires ACK */
CON: Device → Cloud
ACK: Cloud → Device (confirms receipt)

/* Non-confirmable message - fire and forget */
NON: Device → Cloud (no ACK expected)
```

**Example: Sending sensor data**

```
Device                       nRF Cloud
  │                             │
  │──CON POST /sensor/temp─────>│
  │                             │
  │<────ACK (Success)───────────│
  │                             │
```

### 2.4 CoAP in Asset Tracker Template

The template uses nRF Cloud's CoAP library:

```c
#include <net/nrf_cloud_coap.h>

/* Send sensor data */
int err = nrf_cloud_coap_sensor_send(
    "TEMP",                    /* Sensor type */
    25.5,                      /* Value */
    timestamp_ms,              /* Timestamp */
    true                       /* Confirmable */
);

/* Send location */
struct nrf_cloud_gnss_data location = {
    .lat = 63.4305,
    .lon = 10.3951,
    .accuracy = 10.0,
};

err = nrf_cloud_coap_location_send(
    &location,
    true,                      /* Confirmable */
    &timestamp_ms
);
```

## 3. CBOR Encoding

### 3.1 What is CBOR?

**CBOR (Concise Binary Object Representation)** is a binary data format like JSON but more compact:

**JSON Example:**
```json
{
  "temperature": 25.5,
  "humidity": 60.2,
  "timestamp": 1699876543210
}
```
Size: ~72 bytes

**CBOR Equivalent:**
```
A3                          # map(3)
   6B 74656D7065726174757265 # text(11) "temperature"
   F9 4CCD                  # float16(25.5)
   68 68756D6964697479     # text(8) "humidity"
   F9 4E0C                  # float16(60.2)
   69 74696D657374616D70   # text(9) "timestamp"
   1B 0001 8C8F A3E5 BA7A   # uint(1699876543210)
```
Size: ~42 bytes

**Savings: 42%**

### 3.2 Why CBOR?

✅ **Smaller size** - 40-75% reduction vs JSON
✅ **Lower power** - Less data to transmit
✅ **Lower cost** - Reduces cellular data charges
✅ **Faster encoding/decoding** - Binary format
✅ **Type-safe** - Schema validation with CDDL

### 3.3 CBOR with nRF Cloud

nRF Cloud uses **CDDL (Common Data Definition Language)** schemas:

```cddl
; Device shadow message
device_shadow = {
    state: {
        ? desired: state_object
        ? reported: state_object
    }
}

state_object = {
    ? config: config_object
    ? sensors: sensor_data
}

config_object = {
    ? sampleIntervalSeconds: uint
    ? locationTimeout: uint
}
```

The Asset Tracker Template includes CDDL schemas in `src/cbor/`:

```
src/cbor/
├── device_shadow.cddl      # Device shadow schema
├── cbor_helper.c           # CBOR encoding/decoding
└── cbor_helper.h           # Helper functions
```

### 3.4 Using CBOR in ATT

The template automatically encodes data in CBOR:

```c
/* In cloud module (cloud.c) */
static int send_sensor_data(double temperature, int64_t timestamp_ms)
{
    /* nRF Cloud library handles CBOR encoding internally */
    return nrf_cloud_coap_sensor_send(
        "TEMP",
        temperature,
        timestamp_ms,
        true  /* Confirmable */
    );
}
```

**Behind the scenes:**

1. Data is encoded to CBOR using zcbor library
2. CBOR payload is sent via CoAP
3. nRF Cloud decodes CBOR and stores data
4. Data appears in nRF Cloud web UI

## 4. Device Shadows

### 4.1 What is a Device Shadow?

A **device shadow** (also called **digital twin**) is a JSON document in nRF Cloud containing the device state:

```json
{
  "state": {
    "reported": {
      "appVersion": "1.0.0",
      "modemFirmware": "mfw_nrf91x1_2.0.1",
      "config": {
        "sampleIntervalSeconds": 300,
        "locationTimeout": 60
      }
    },
    "desired": {
      "config": {
        "sampleIntervalSeconds": 600,
        "locationTimeout": 120
      }
    }
  }
}
```

### 4.2 Shadow Sections

| Section | Purpose | Updated By |
|---------|---------|------------|
| **reported** | Current device state | Device |
| **desired** | Target device state | Cloud/User |
| **delta** | Difference between reported and desired | Computed by cloud |

### 4.3 Configuration Workflow

**Updating device configuration via shadow:**

```
1. USER UPDATES DESIRED STATE
   User (Web UI) → nRF Cloud
   Sets desired.config.sampleIntervalSeconds = 600

2. DEVICE POLLS SHADOW
   Device → GET /v1/shadow/delta
   Receives: {"sampleIntervalSeconds": 600}

3. DEVICE APPLIES CONFIGURATION
   Device updates runtime configuration

4. DEVICE REPORTS NEW STATE
   Device → POST /v1/shadow/state
   Updates: reported.config.sampleIntervalSeconds = 600

5. DELTA CLEARED
   nRF Cloud clears delta (reported == desired)
```

### 4.4 Working with Shadows in ATT

The cloud module handles shadow operations:

```c
/* Fetch shadow delta */
static int fetch_shadow_delta(void)
{
    struct nrf_cloud_data shadow_data = {0};
    
    /* Get shadow delta from nRF Cloud */
    int err = nrf_cloud_coap_shadow_get(
        &shadow_data,
        true,    /* Get delta only */
        COAP_CONTENT_FORMAT_APP_CBOR
    );
    
    if (err == 0 && shadow_data.len > 0) {
        /* Parse and apply configuration */
        parse_shadow_delta(shadow_data.ptr, shadow_data.len);
    }
    
    return err;
}

/* Update shadow with reported state */
static int update_shadow_reported(void)
{
    struct nrf_cloud_obj obj;
    
    /* Create shadow object */
    nrf_cloud_obj_init(&obj);
    nrf_cloud_obj_shadow_update(&obj);
    
    /* Add reported state */
    nrf_cloud_obj_int_add(&obj, "sampleIntervalSeconds",
                          current_sample_interval);
    
    /* Send to cloud */
    int err = nrf_cloud_coap_obj_send(&obj, true);
    
    nrf_cloud_obj_free(&obj);
    return err;
}
```

### 4.5 Configuring Device from nRF Cloud

**Via Web UI:**

1. Navigate to your device in nRF Cloud
2. Click **Device Shadow** tab
3. Edit the **desired** section:
   ```json
   {
     "config": {
       "sampleIntervalSeconds": 600,
       "locationTimeout": 120,
       "activeMode": true
     }
   }
   ```
4. Click **Save**
5. Device will fetch and apply configuration on next shadow poll

## 5. Data Flow

### 5.1 Complete Data Flow

```
1. SENSOR SAMPLING
   Sensor → Environmental Module
   BME680 samples temperature/humidity/pressure

2. DATA COLLECTION
   Environmental Module → Storage Module
   Message: ENVIRONMENTAL_SAMPLE_RESPONSE

3. DATA STORAGE
   Storage Module → Flash (if buffering)
   OR
   Storage Module → Cloud Module (if connected)

4. CBOR ENCODING
   Cloud Module encodes data to CBOR

5. CoAP TRANSMISSION
   Cloud Module → nRF Cloud via CoAP/DTLS
   POST /v1/message

6. CLOUD PROCESSING
   nRF Cloud receives, decodes, stores data

7. USER ACCESS
   User views data in nRF Cloud Web UI
```

### 5.2 Storage Module Modes

The storage module operates in two modes:

**Passthrough Mode (Cloud Connected):**
```
Sensor Data ──> Storage Module ──> Cloud Module ──> nRF Cloud
                (immediate)
```

**Buffer Mode (Cloud Disconnected):**
```
Sensor Data ──> Storage Module ──> Flash Memory
                (buffered)

Later when connected:
Flash Memory ──> Storage Module ──> Cloud Module ──> nRF Cloud
```

### 5.3 Data Types

The storage module handles multiple data types:

```c
enum storage_data_type {
    STORAGE_TYPE_LOCATION,           /* GPS coordinates */
    STORAGE_TYPE_ENVIRONMENTAL,      /* Temperature, humidity, pressure */
    STORAGE_TYPE_POWER,             /* Battery voltage, percentage */
    STORAGE_TYPE_NETWORK_QUALITY,    /* RSRP, cell ID */
};

struct storage_item {
    enum storage_data_type type;
    int64_t timestamp_ms;
    
    union {
        struct location_msg LOCATION;
        struct environmental_msg ENVIRONMENTAL;
        struct power_msg POWER;
        struct network_quality_msg NETWORK_QUALITY;
    } data;
};
```

### 5.4 Sending Custom Data

To send custom data to nRF Cloud:

```c
/* Option 1: Use predefined sensor types */
nrf_cloud_coap_sensor_send(
    "CUSTOM_SENSOR",     /* App ID */
    value,               /* Double value */
    timestamp_ms,
    true                 /* Confirmable */
);

/* Option 2: Send raw message */
nrf_cloud_coap_message_send(
    "MY_DATA_TYPE",      /* App ID */
    message_string,      /* Data as string */
    false,               /* Binary data? */
    timestamp_ms,
    true                 /* Confirmable */
);
```

**Example: Send string data**

```c
char message[64];
snprintf(message, sizeof(message), "Alert: Temperature %.2f exceeds threshold", temp);

nrf_cloud_coap_message_send(
    "TEMP_ALERT",
    message,
    false,              /* Not binary */
    k_uptime_get(),
    true
);
```

## 6. Firmware Over-The-Air (FOTA) Updates

### 6.1 FOTA Overview

**FOTA** allows updating device firmware remotely over cellular:

**Benefits:**
- ✅ Update devices in the field
- ✅ Fix bugs without physical access
- ✅ Add new features
- ✅ Update modem firmware
- ✅ Rollback if update fails

### 6.2 FOTA Architecture

```
┌──────────────┐
│  nRF Cloud   │ ◄── 1. Upload firmware bundle
└──────┬───────┘
       │
       │ 2. Create FOTA job
       │
       ▼
┌──────────────┐
│   Device     │ ◄── 3. Poll for updates
└──────┬───────┘
       │
       │ 4. Download firmware
       │
       ▼
┌──────────────┐
│   MCUboot    │ ◄── 5. Verify and apply
└──────┬───────┘
       │
       │ 6. Reboot to new firmware
       │
       ▼
┌──────────────┐
│  New Version │ ◄── 7. Report version
└──────────────┘
```

### 6.3 FOTA Update Types

| Type | Updates | Reboot Required |
|------|---------|-----------------|
| **Application** | Your application code | Yes |
| **Modem** | nRF91 modem firmware | Yes |
| **Bootloader** | MCUboot bootloader | Yes |

### 6.4 Performing FOTA Updates

**Step 1: Prepare Firmware**

Edit `app/VERSION`:
```
VERSION_MAJOR = 1
VERSION_MINOR = 1
PATCHLEVEL = 0
VERSION_TWEAK = 0
EXTRAVERSION =
```

Build:
```bash
west build -p -b thingy91x/nrf9151/ns
```

Locate update bundle:
```
build/app/zephyr/dfu_application.zip
```

**Step 2: Upload to nRF Cloud**

1. Navigate to **Device Management** → **Firmware Updates**
2. Click **Add Bundle**
3. Upload `dfu_application.zip`
4. Set **Update Type**: `APP`
5. Set **Name**: `ATT v1.1.0`
6. Set **Version**: `1.1.0`
7. Click **Create/Upload Bundle**

**Step 3: Create FOTA Job**

1. Click **Create FOTA Update**
2. Enter **Name** and **Description**
3. Select **Target Devices**
4. Select **Bundle**: `ATT v1.1.0`
5. Click **Deploy Now**
6. Click **Create FOTA Update**

**Step 4: Monitor Update**

Device will:
1. Poll for updates (automatically)
2. Download firmware
3. Verify and install
4. Reboot
5. Report new version

Watch device logs:
```
[00:10:15.123] <inf> fota: FOTA update available: v1.1.0
[00:10:15.456] <inf> fota: Downloading firmware...
[00:12:34.789] <inf> fota: Download complete, applying update
[00:12:35.012] <inf> fota: Update successful, rebooting
*** Booting nRF Connect SDK v3.1.0 ***
[00:00:00.123] <inf> main: Asset Tracker Template v1.1.0 started
```

### 6.5 FOTA Module State Machine

```
STATE_RUNNING
├── STATE_IDLE
│   └─── Waiting for FOTA check trigger
├── STATE_CHECKING
│   └─── Polling nRF Cloud for updates
├── STATE_DOWNLOADING
│   └─── Downloading firmware image
├── STATE_DOWNLOADED
│   └─── Download complete, ready to apply
└── STATE_APPLYING
    └─── Installing update, will reboot
```

### 6.6 FOTA Configuration

```kconfig
# Enable FOTA module
CONFIG_APP_FOTA=y

# nRF Cloud FOTA support
CONFIG_NRF_CLOUD_FOTA=y

# FOTA download
CONFIG_FOTA_DOWNLOAD=y
CONFIG_FOTA_DOWNLOAD_PROGRESS_EVT=y

# Bootloader (MCUboot)
CONFIG_BOOTLOADER_MCUBOOT=y
CONFIG_IMG_MANAGER=y
```

## 7. Debugging Cloud Connectivity

### 7.1 Enable Detailed Logging

```kconfig
# Cloud module logging
CONFIG_APP_CLOUD_LOG_LEVEL_DBG=y

# nRF Cloud CoAP library logging
CONFIG_NRF_CLOUD_COAP_LOG_LEVEL_DBG=y

# Network logging
CONFIG_NET_LOG=y
CONFIG_NET_SOCKETS_LOG_LEVEL_DBG=y
```

### 7.2 Common Issues

**Problem: Device not connecting to cloud**

Check:
1. Network connectivity
   ```bash
   uart:~$ at at+cereg?
   +CEREG: 2,1,"XXXX","XXXXXXXX",7
   # Should show registered (1 or 5)
   ```

2. Cloud credentials provisioned
   ```
   [cloud] <inf> Device claimed: yes
   [cloud] <inf> Credentials provisioned: yes
   ```

3. Time synchronization
   ```
   [cloud] <inf> Time synchronized from network
   ```

**Problem: Data not appearing in nRF Cloud**

Check:
1. Data being sent
   ```
   [cloud] <dbg> Sending temperature: 25.5
   [cloud] <dbg> CoAP response: 2.04 Changed
   ```

2. Device is online in nRF Cloud UI

3. Correct nRF Cloud account

**Problem: FOTA update fails**

Check:
1. Sufficient storage space
   ```kconfig
   CONFIG_PM_PARTITION_SIZE_MCUBOOT_SECONDARY=0x80000  # 512 KB
   ```

2. Firmware bundle type matches device

3. Network stability during download

### 7.3 Shell Commands

Useful shell commands for debugging:

```bash
# Check cloud connection
uart:~$ kernel threads | grep cloud

# Check network status
uart:~$ at at+cereg?
uart:~$ at at+cesq

# Manually trigger cloud sync
uart:~$ att cloud_sync

# Check firmware version
uart:~$ kernel version
```

## 8. Best Practices

### 8.1 Data Transmission

✅ **DO:**
- Batch data when possible to reduce overhead
- Use confirmable messages for critical data
- Implement retry logic with exponential backoff
- Buffer data when offline

❌ **DON'T:**
- Send data too frequently (wastes power and data)
- Ignore CoAP error responses
- Send large payloads (fragment if needed)

### 8.2 Device Shadows

✅ **DO:**
- Poll shadows periodically (not too frequently)
- Validate configuration before applying
- Report applied configuration back to cloud
- Handle configuration errors gracefully

❌ **DON'T:**
- Poll shadows on every sample (wasteful)
- Apply configuration without validation
- Forget to update reported state
- Block on shadow operations

### 8.3 FOTA Updates

✅ **DO:**
- Test updates thoroughly before deployment
- Increment version numbers appropriately
- Monitor update progress in nRF Cloud
- Implement rollback mechanism

❌ **DON'T:**
- Deploy untested firmware
- Skip version validation
- Forget to update VERSION file
- Ignore MCUboot configuration

## Summary

In this lesson, you learned:

✅ CoAP protocol provides efficient communication for cellular IoT
✅ CBOR encoding reduces data usage by 40-75% compared to JSON
✅ Device shadows enable remote configuration management
✅ Storage module handles data buffering for offline operation
✅ FOTA enables remote firmware updates over cellular
✅ nRF Cloud provides integrated device management and services
✅ Proper debugging techniques help troubleshoot connectivity issues

## Exercises

### Exercise 1: Send Custom Data to nRF Cloud

Implement a custom sensor or alert and send the data to nRF Cloud using CoAP.

See [Exercise 1 instructions](../exercises/lesson5/exercise1/README.md)

### Exercise 2: Implement Data Buffering

Configure and test the storage module's buffering capability for offline operation.

See [Exercise 2 instructions](../exercises/lesson5/exercise2/README.md)

## Next Steps

In the next lesson, you'll learn about **Customization and Adding New Features** - including adding sensors, creating custom modules, implementing power optimization, and using alternative cloud backends.

Continue to [Lesson 6: Customization and Adding New Features](lesson6_customization.md)

