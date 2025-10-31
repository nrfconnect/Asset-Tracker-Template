# Configuration

The following sections explain different configuration needed for the Asset tracker template:

## Set sampling interval and logic from cloud

The Asset Tracker can be configured remotely through nRF Cloud's device shadow mechanism. This allows dynamic adjustment of device behavior without requiring firmware updates.

### Configuration through nRF Cloud UI

> [!NOTE]
> For new devices, the **View Config** section in the nRF Cloud UI will not be visible. It will become visible once the shadow is patched using the REST call documented below.

1. Log in to [nRF Cloud](https://nrfcloud.com/).
1. Navigate to **Devices** and select your device.
1. Click on **View Config** on the top bar.
1. Select **Edit Configuration**.
1. Enter the desired configuration:

    ```json
    {
    "update_interval": 60
    }
    ```

1. Click **Commit** to apply the changes.

The device receives the new configuration through its shadow and adjust its update interval accordingly.

### Configuration through REST API

You can update the interval using [nRF Cloud REST API](https://api.nrfcloud.com/#tag/IP-Devices/operation/UpdateDeviceState).

```
curl -X PATCH   "https://api.nrfcloud.com/v1/devices/$DEVICE_ID/state"   -H "Authorization: Bearer $API_KEY"   -H "Content-Type: application/json"   -d '{ "desired": { "config": { "update_interval": <your_value> } } }'
```

### Sending commands through REST API

Send device commands using the [nRF Cloud REST API](https://api.nrfcloud.com/#tag/IP-Devices/operation/UpdateDeviceState):

```bash
curl -X PATCH "https://api.nrfcloud.com/v1/devices/$DEVICE_ID/state" \
-H "Authorization: Bearer $API_KEY" \
-H "Content-Type: application/json" \
-d '{"desired": {"command": [1, 1]}}'
```

**Command format**: `"command": [type, id]`

- **type**: Command type (0=Reboot, 1=Provision)
- **id**: Unique identifier (increment for successive commands)

*For shadow structure details, see `Asset-Tracker-Template/app/src/cbor/device_shadow.cddl`*

### Configuration Flow

* **Initial Setup**

    - The device starts with default interval from `CONFIG_APP_MODULE_TRIGGER_TIMEOUT_SECONDS`.
    - Upon cloud connection, the device automatically requests shadow configuration.

* **Runtime Configuration**

    - Cloud module receives and processes shadow updates.
    - Device maintains last known configuration during offline periods.

* **Impact on Device Behavior**

    The `update_interval` configuration controls the frequency of:

    - Location updates
    - Sensor sampling (environmental, battery, network quality)
    - FOTA update checks
    - Shadow update polling

## Set location method priorities

The Asset Tracker supports multiple location methods that can be prioritized based on your needs. Configuration is done through board-specific configuration files.

### Available Location Methods

The following are the available location methods:

- GNSS (GPS)
- Wi-Fi® positioning
- Cellular positioning

### Configuration Examples

* **Thingy91x Configuration** (Wi-Fi available):

    ```
    CONFIG_LOCATION_REQUEST_DEFAULT_METHOD_FIRST_WIFI=y
    CONFIG_LOCATION_REQUEST_DEFAULT_METHOD_SECOND_GNSS=y
    CONFIG_LOCATION_REQUEST_DEFAULT_METHOD_THIRD_CELLULAR=y
    CONFIG_LOCATION_REQUEST_DEFAULT_WIFI_TIMEOUT=10000
    ```

* **nRF9151 DK Configuration** (Wi-Fi unavailable):

    ```
    CONFIG_LOCATION_REQUEST_DEFAULT_METHOD_FIRST_GNSS=y
    CONFIG_LOCATION_REQUEST_DEFAULT_METHOD_SECOND_CELLULAR=y
    ```

## Storage Mode Configuration

The storage module handles collected data in two modes: **Passthrough** (forward immediately, default) or **Buffer** (store and transmit in batches for lower power consumption).
See [Storage Module Documentation](../modules/storage.md) for details.

**Basic configuration** in `prj.conf`:

Passthrough mode is the default mode, to enable buffer mode use:

```bash
CONFIG_APP_STORAGE_INITIAL_MODE_BUFFER=y
```

To configure buffer size and records per stored data type:

```bash
CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE=8      # Records per data type
CONFIG_APP_STORAGE_BATCH_BUFFER_SIZE=256       # Batch buffer size
```

For minimal use include the `overlay-storage-minimal.conf` overlay

**Runtime control** (shell commands when `CONFIG_APP_STORAGE_SHELL=y`):

```bash
att_storage mode passthrough   # Switch to passthrough
att_storage mode buffer        # Switch to buffer
att_storage flush              # Flush stored data
att_storage clear              # Clear all data
att_storage stats              # Show statistics (if enabled)
```

See [Storage Module Configurations](../modules/storage.md#configurations) for all options.

## Network configuration

### NB-IoT vs LTE-M

The Asset Tracker supports both LTE Cat NB1 (NB-IoT) and LTE Cat M1 (LTE-M) cellular connectivity:

- **NB-IoT**: Optimized for:

    - Low data rate applications.
    - Better coverage.
    - Stationary or low-mobility devices.

- **LTE-M**: Better suited for:

    - Higher data rates.
    - Mobile applications.
    - Lower latency requirements.

#### Network Mode Selection

The following network modes are available (`LTE_NETWORK_MODE`):

- **Default**: Use the system mode currently set in the modem.
- **LTE-M**: LTE Cat M1 only.
- **LTE-M and GPS**: LTE Cat M1 with GPS enabled.
- **NB-IoT**: NB-IoT only.
- **NB-IoT and GPS**: NB-IoT with GPS enabled.
- **LTE-M and NB-IoT**: Both LTE-M and NB-IoT enabled.
- **LTE-M, NB-IoT and GPS**: Both LTE modes with GPS .

#### Network Mode Preference

When multiple network modes are enabled (LTE-M and NB-IoT), you can set preferences (`LTE_MODE_PREFERENCE`):

- **No preference**: Automatically selected by the modem.
- **LTE-M**: Prioritize LTE-M over PLMN selection.
- **NB-IoT**: Prioritize NB-IoT over PLMN selection.
- **LTE-M, PLMN prioritized**: Prefer LTE-M but prioritize staying on home network.
- **NB-IoT, PLMN prioritized**: Prefer NB-IoT but prioritize staying on home network.

Example configuration in `prj.conf`:

```
# Enable both LTE-M and NB-IoT with GPS
CONFIG_LTE_NETWORK_MODE_LTE_M_NBIOT_GPS=y

# Prefer LTE-M while prioritizing home network
CONFIG_LTE_MODE_PREFERENCE_LTE_M_PLMN_PRIO=y
```

### PSM (Power Saving Mode)

PSM allows the device to enter deep sleep while maintaining network registration. Configuration is done through Kconfig options:

#### PSM Parameters

* **Periodic TAU (Tracking Area Update)**

    - Controls how often the device updates its location with the network
    - Configuration options:

        ```
        # Configure TAU in seconds
        CONFIG_LTE_PSM_REQ_RPTAU_SECONDS=1800  # 30 minutes
        ```

* **Active Time (RAT)**

    - Defines how long the device stays active after a wake-up
    - Configuration options:

        ```
        # Configure RAT in seconds
        CONFIG_LTE_PSM_REQ_RAT_SECONDS=60  # 1 minute
        ```

The following are the Key aspects:

- Device negotiates PSM parameters with the network.
- Helps achieve longer battery life.
- Device remains registered but unreachable during sleep.
- Wakes up periodically based on TAU setting.
- Stays active for the duration specified by RAT.


### APN (Access Point Name)

The Access Point Name (APN) is a network identifier used by the device to connect to the cellular network's packet data network. Configuration options:

- **Default APN**: Most carriers automatically configure the correct APN.
- **Manual Configuration**: If needed, APN can be configured through Kconfig:

    ```
    CONFIG_PDN_DEFAULT_APN="Access point name"
    ```

Common scenarios for APN configuration:

- Using a custom/private APN.
- Connecting to specific network services.
- Working with MVNOs (Mobile Virtual Network Operators).

> [!NOTE]
> In most cases, the default APN provided by the carrier should work without additional configuration.

## LED Status Indicators

The Asset Tracker Template uses LED colors to indicate different device states:

- **Yellow** (Blinking, 10 repetitions): Device is idle and disconnected from cloud.
- **Green** (Blinking, 10 repetitions): Device is connected to cloud and actively sampling data.
- **Blue** (Blinking, 10 repetitions): Device is in lower power mode state between samples.
- **Purple** (Blinking, 10 repetitions): FOTA download in progress.

### Example: Setting LED Colors

You can control the LED colors through the LED module using zbus messages. The following is an example of how to set different LED patterns:

```c
/* Set yellow blinking pattern for idle state */
struct led_msg led_msg = {
    .type = LED_RGB_SET,
    .red = 255,
    .green = 255,
    .blue = 0,
    .duration_on_msec = 250,
    .duration_off_msec = 2000,
    .repetitions = 10,
};

/* Publish the message to LED_CHAN */
int err = zbus_chan_pub(&LED_CHAN, &led_msg, K_SECONDS(1));
if (err) {
    LOG_ERR("zbus_chan_pub, error: %d", err);
    return;
}
```

The LED message structure allows you to:

- Set RGB values for color (`0-255` for each component).
- Define on/off durations in milliseconds.
- Specify number of repetitions (`-1` for continuous blinking).
