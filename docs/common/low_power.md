# Achieving low power

This page provides an overview of how to achieve low power consumption with the Asset Tracker Template application. It covers the fundamental concepts, the measurement tools, the configuration options, and the best practices for optimizing battery life in cellular IoT applications.

## Prerequisites

If you are not familiar with cellular IoT fundamentals and low-power concepts, first complete the [Cellular IoT Fundamentals](https://academy.nordicsemi.com/courses/cellular-iot-fundamentals/) course in the Nordic Developer Academy. The course covers the following essential topics:

- The basics of the LTE-M and NB-IoT network technologies.
- The power saving modes, which are power saving mode (PSM) and extended discontinuous reception (eDRX).

It is also recommended to read the [Best Practices for Cellular Development](https://docs.nordicsemi.com/bundle/nwp_044/page/WP/nwp_044/intro.html) documentation.

## Measuring power consumption

To understand your device's actual power consumption and optimize the battery life, use the following power profiling tools.

### Online Power Profiler for LTE

For quick estimates and profiling of current consumption based on network and transmission parameters, use the [Online Power Profiler for LTE](https://devzone.nordicsemi.com/power/w/opp/3/online-power-profiler-for-lte).

### Power Profiler Kit 2

For accurate measurements and detailed profiling, use the [Power Profiler Kit 2](https://www.nordicsemi.com/Products/Development-hardware/Power-Profiler-Kit-2) (PPK2). For detailed guidance on how to use the PPK2, see the [Power Profiler Kit user guide](https://docs.nordicsemi.com/bundle/ug_ppk2/page/UG/ppk/PPK_user_guide_Intro.html).

## Application power configuration

The Asset Tracker Template is configured with power-saving features enabled by default. The following sections cover the key configuration options that are specific to the application.

For general configuration of the LTE link, see the [LTE Link Control](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/libraries/modem/lte_lc.html) documentation.

### Power saving mode (PSM)

PSM is enabled by default. Configure the tracking area update (TAU) and requested active time (RAT) parameters in `prj.conf`:

```config
CONFIG_LTE_PSM_REQ_RPTAU_SECONDS=7200  # Periodic TAU: 2 hours
CONFIG_LTE_PSM_REQ_RAT_SECONDS=6       # Active Time: 6 seconds
```

Configure a periodic TAU timer that is longer than the application's update cycle, which is the sampling interval multiplied by the storage threshold. This avoids synchronization with the
network between uploads.

For additional options, see [Power saving mode (PSM)](configuration.md#power-saving-mode-psm) in the configuration documentation. The device negotiates the actual values with the network.

### Extended discontinuous reception (eDRX)

eDRX is enabled by default. Configure the eDRX interval in `prj.conf`:

```config
CONFIG_LTE_EDRX_REQ_VALUE_LTE_M="0000"  # eDRX interval 5.12 s for LTE-M
CONFIG_LTE_EDRX_REQ_VALUE_NBIOT="0000"  # eDRX interval 5.12 s for NB-IoT
```

The modem uses eDRX to sleep periodically during the PSM active timer. With the default settings, the device monitors for downlink data every 5.12 seconds for the duration of the active timer, which is six seconds by default, in case the cloud sends data to the device.

### Sampling and send behavior

The following Kconfig options set in the `prj.conf` file control sampling and
transmission frequency:

```config
CONFIG_APP_SAMPLING_INTERVAL_SECONDS=600      # Default: 10 minutes
CONFIG_APP_STORAGE_INITIAL_THRESHOLD=1        # Default: 1 item
```

The application samples and stores the sensor and location data at the interval set by the
`CONFIG_APP_SAMPLING_INTERVAL_SECONDS` Kconfig option. It sends the buffered data to the cloud
when the number of stored items reaches
`CONFIG_APP_STORAGE_INITIAL_THRESHOLD`.

You can adjust these values at runtime using the nRF Cloud device shadow. For more information, see
[Runtime configurations](configuration.md#runtime-configurations) in the configuration documentation.

Longer intervals between operations and a higher storage threshold result in fewer network connections, which lowers the power consumption.

Set the PSM periodic TAU interval reasonably longer than the
expected cloud upload cadence, which is the sampling interval multiplied by the storage threshold. This avoids waking the modem up too often for
network maintenance.

### UART power management

UART interfaces consume power even when idle. The application provides options to disable the UART at runtime to reduce the power consumption.

#### Automatic UART disable on Thingy:91 X

The Thingy:91 X can disable the UART automatically when the USB cable is disconnected. Enable the feature in `prj.conf`:

```config
CONFIG_APP_UART_POWER_CONTROL=y
```

When the feature is enabled, the device monitors VBUS and suspends the UART interfaces automatically when the USB power is removed. This is useful during development, where you need UART logging while USB is connected but want to minimize the power consumption when the device runs on battery. For more information, see [UART Power Control module](../modules/uart_power_control.md).

#### Manual UART control using shell

You can suspend the UART devices manually at runtime using the following shell commands:

```bash
uart:~$ pm suspend uart@9000  # Suspend UART0
uart:~$ pm suspend uart@8000  # Suspend UART1
```

> [!NOTE]
> After suspending the UART that you use for the shell, you cannot send further commands until you reset the device or resume the UART through other means.

### Wi-Fi scanning optimization on Thingy:91 X using the nRF7002

The Thingy:91 X uses Wi-Fi® scanning through the nRF7002 for location services. A full Wi-Fi scan across all bands can take 5 to 10 seconds, which consumes significant power. To reduce the scan time and the power consumption, the application restricts the scanning to the 2.4 GHz band by default.

For detailed information about the Wi-Fi scan timing, channels, and dwell times, see the [Wi-Fi scan operation](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/protocols/wifi/scan_mode/scan_operation.html) documentation.

#### 2.4 GHz band scanning

The application sets `CONFIG_NRF_WIFI_2G_BAND` by default to restrict the Wi-Fi scanning to the 2.4 GHz band. This is a deliberate optimization for location services, for the following reasons:

- Most commercial access points and hotspots use channels 1, 6, and 11, which are the standard non-overlapping 2.4 GHz channels. Scanning the 2.4 GHz band provides sufficient access point coverage for location services in the vast majority of applications.
- The application uses active scanning by default, which is preferred for moving devices. Because the device might not stay in range of an access point for long, active scanning, which sends probe requests and receives probe responses, discovers access points significantly faster than passive scanning, which waits for beacons.

If your use case needs 5 GHz access point coverage, remove `CONFIG_NRF_WIFI_2G_BAND=y` from the board configuration file, for example `boards/thingy91x_nrf9151_ns.conf`.

## Optimization best practices

To optimize the power consumption of your application, apply the following practices:

1. **Disable the peripherals**: Disable the UART and the peripherals that consume a lot of power.
1. **Increase the sampling interval**: Reduce the sampling frequency.
1. **Increase the storage threshold**: Store more data before sending it to the cloud.
1. **Minimize the payload size**: Reduce the transmission duration by using effective serialization formats, such as CBOR or Protobuf.
1. **Validate with the PPK2**: Measure the actual power consumption.

## Additional resources

The following resources provide more information about power optimization:

- [Cellular IoT Fundamentals](https://academy.nordicsemi.com/courses/cellular-iot-fundamentals/)
- [Tooling and troubleshooting](tooling_troubleshooting.md)
- [Configuration](configuration.md)
- [Storage module](../modules/storage.md)
- [Network module](../modules/network.md)
- [Optimizing power consumption](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/test_and_optimize/optimizing/power.html)
- [LTE Link Control](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/libraries/modem/lte_lc.html)
