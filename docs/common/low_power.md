# Achieving low power

This section provides an overview of how to achieve low power consumption with the Asset Tracker Template application. It covers fundamental concepts, measurement tools, configuration options, and best practices for optimizing battery life in cellular IoT applications.

## Prerequisites

If you are not familiar with cellular IoT fundamentals and low-power concepts, it is highly recommended to first complete the [Cellular IoT Fundamentals Developer Academy Course](https://academy.nordicsemi.com/courses/cellular-iot-fundamentals/). This course covers essential topics including:

- LTE-M and NB-IoT network technologies basics
- Power saving modes (PSM, eDRX)

It is also recommended to read the [Best Practices for Cellular Development](https://docs.nordicsemi.com/bundle/nwp_044/page/WP/nwp_044/intro.html) documentation.

## Measuring power consumption

The following Power Profiling tools are recommended to understand your device's actual power consumption for optimizing battery life:

### Online Power Profiler for LTE

For quick estimates and profiling of current consumption based on network and transmission parameters, use the [Online Power Profiler for LTE](https://devzone.nordicsemi.com/power/w/opp/3/online-power-profiler-for-lte).

### Power Profiler Kit 2 (PPK2)

For accurate measurements and detailed profiling, use the [PPK2: Power Profiler Kit 2](https://www.nordicsemi.com/Products/Development-hardware/Power-Profiler-Kit-2) and for detailed guidance on how to use the PPK2, see the [Power Profiler Kit User Guide](https://docs.nordicsemi.com/bundle/ug_ppk2/page/UG/ppk/PPK_user_guide_Intro.html).

## Application power configuration

The Asset Tracker Template is configured with power-saving features enabled by default. This section covers the key configuration options specific to the application.

For general configuration of the LTE link, refer to the [LTE Link Control documentation](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/libraries/modem/lte_lc.html).

### Power Saving Mode (PSM)

PSM is **enabled by default**. Configure TAU and RAT parameters in `prj.conf`:

```config
CONFIG_LTE_PSM_REQ_RPTAU_SECONDS=1800  # Periodic TAU: 30 minutes
CONFIG_LTE_PSM_REQ_RAT_SECONDS=60      # Active Time: 1 minute
```

It is recommended to configure a periodic TAU timer longer than the update interval of the application to avoid synchronization with the network between updates.

See [Configuration guide - PSM](configuration.md#psm-power-saving-mode) for additional options. The actual values are negotiated with the network.

### Extended Discontinuous Reception (eDRX)

eDRX is **enabled by default**. Configure the eDRX interval in `prj.conf`:

```config
CONFIG_LTE_EDRX_REQ_VALUE_LTE_M="0000"  # eDRX interval 5.12 s for LTE-M
CONFIG_LTE_EDRX_REQ_VALUE_NBIOT="0000"  # eDRX interval 5.12 s for NB-IoT
```

eDRX is used to periodically sleep during the PSM active timer. With the default settings, the device monitors for downlink data every 5.12 seconds for the duration of the active timer (60 seconds by default) in case the cloud sends data to the device.

### Update interval

The following Kconfig options that are set in the `prj.conf` file control sampling and transmission frequency:

```config
CONFIG_APP_SAMPLING_INTERVAL_SECONDS=150    # Default: 2.5 minutes
CONFIG_APP_CLOUD_UPDATE_INTERVAL_SECONDS=600            # Default: 10 minutes
CONFIG_APP_STORAGE_INITIAL_THRESHOLD=1 # Default: 1 item
```

Sensors and location are sampled and stored at every interval set in the `CONFIG_APP_SAMPLING_INTERVAL_SECONDS` Kconfig option and sent to the cloud at every interval as set in the `CONFIG_APP_CLOUD_UPDATE_INTERVAL_SECONDS` Kconfig option. Or when the number of stored items reaches the `CONFIG_APP_STORAGE_INITIAL_THRESHOLD` threshold.

You can adjust these Kconfig options at runtime using the nRF Cloud device shadow. See [Configuration guide](configuration.md#set-sampling-interval-and-logic-from-cloud) for details.

**Impact:** Using longer intervals between operations and increasing the storage threshold results in fewer network connections and leads to lower power consumption.

Ensure that the PSM Periodic TAU interval is set reasonably longer than the cloud sync interval to avoid waking the modem up to perform tracking or updates.

### UART power management

UART interfaces consume power even when idle. The application provides options to disable UART at runtime to reduce power consumption.

#### Automatic UART disable on Thingy:91 X

The Thingy:91 X can automatically disable UART when the USB cable is disconnected. Enable in `prj.conf`:

```config
CONFIG_APP_POWER_DISABLE_UART_ON_VBUS_REMOVED=y
```

When enabled, the device monitors VBUS and automatically suspends UART interfaces when USB power is removed. This is useful for development where you need UART logging during USB connection but want to minimize power consumption when running on battery.

#### Manual UART control using shell

UART devices can be manually suspended at runtime using shell commands:

```bash
uart:~$ pm suspend uart@9000  # Suspend UART0
uart:~$ pm suspend uart@8000  # Suspend UART1
```

> [!NOTE]
> After suspending the UART you are using for the shell, you will not be able to send further commands until the device is reset or the UART is resumed through other means.

### Wi-Fi scanning optimization (Thingy:91 X using the nRF7002)

On Thingy:91 X, Wi-Fi scanning is used for location services. Without optimization, a full scan across all bands takes ~4.65 s at ~55 mA average current. The application is configured to minimize scan time and power consumption by using passive scanning on only the three most common 2.4 GHz channels.

For detailed information on Wi-Fi scan profiles and their power characteristics, see the [Wi-Fi scan operation documentation](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/protocols/wifi/scan_mode/scan_operation.html).

#### Default scan configuration

The application overrides the default nRF7002 scan behavior with the following settings in `boards/thingy91x_nrf9151_ns.conf`:

```config
CONFIG_LOCATION_METHOD_WIFI_SCANNING_PARAMS_OVERRIDE=y
CONFIG_LOCATION_METHOD_WIFI_SCANNING_PASSIVE=y
CONFIG_LOCATION_METHOD_WIFI_SCANNING_COMMON_CHANNELS=y
```

- **Passive scanning** (`CONFIG_LOCATION_METHOD_WIFI_SCANNING_PASSIVE`) - The Wi-Fi chip only listens for beacons and does not transmit probe requests, reducing peak current draw. This is important for battery-powered devices where high current spikes affect battery voltage and lifetime.
- **Common channels only** (`CONFIG_LOCATION_METHOD_WIFI_SCANNING_COMMON_CHANNELS`) - Restricts scanning to channels 1, 6, and 11, the three non-overlapping 2.4 GHz channels where the vast majority of APs operate. This drastically reduces scan time compared to a full band scan.
- **Passive dwell time: 260 ms** (Kconfig default) - Set to cover at least two beacon intervals (102.4 ms beacon interval + ~30 ms channel contention) × 2, providing reliable AP detection for a mobile asset tracker.

#### Customizing the scan configuration

The scan behavior can be adjusted in your board configuration file (e.g., `boards/thingy91x_nrf9151_ns.conf`):

- To reduce dwell time to a single beacon interval (~130 ms), trading some scan robustness for faster scans:

```config
CONFIG_LOCATION_METHOD_WIFI_SCANNING_DWELL_TIME_PASSIVE=130
```

- To use the default mix of active and passive scanning depending on channel and regulatory domain (faster per channel but transmits probe requests where allowed):

```config
CONFIG_LOCATION_METHOD_WIFI_SCANNING_PASSIVE=n
```

- To scan all 2.4 GHz channels instead of only 1, 6, and 11 (more thorough but slower):

```config
CONFIG_LOCATION_METHOD_WIFI_SCANNING_COMMON_CHANNELS=n
```

## Optimization best practices

1. **Disable peripherals** - Disable UART and peripherals that consume a lot of power.
2. **Increase update intervals** - Reduce sampling/transmission frequency.
3. **Increase storage threshold** - Store more data before sending to cloud.
4. **Minimize payload size** - Reduce transmission duration by using effective serialization formats (CBOR, Protobuf).
5. **Validate with PPK2** - Measure actual power consumption.

## Additional resources

- [Cellular IoT Fundamentals Developer Academy Course](https://academy.nordicsemi.com/courses/cellular-iot-fundamentals/)
- [Tooling and Troubleshooting Guide](tooling_troubleshooting.md)
- [Configuration Guide](configuration.md)
- [Storage Module Documentation](../modules/storage.md)
- [Network Module Documentation](../modules/network.md)
- [nRF Connect SDK Power Optimization Guide](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/test_and_optimize/optimizing/power.html)
- [LTE Link Control Documentation](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/libraries/modem/lte_lc.html)
