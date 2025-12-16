# Asset Tracker Template - NTN Use Case

> **Note:** The NTN use case variant of the Asset Tracker Template is experimental and intended for development and testing purposes.

## Related Variant

- [TN-NTN use case (`tn_ntn_usecase` branch)](https://github.com/nrfconnect/Asset-Tracker-Template/tree/tn_ntn_usecase)

## Overview

The NTN use case application operates in the following cycle:

1. **GNSS Cold Start**: Acquires a GNSS location fix.
2. **NTN Connection**: Switches to NTN system mode and establishes a connection using the acquired location.
3. **Data Transmission**: Sends data to the configured cloud endpoint.
4. **Cycle**: Repeats the process, triggered by a button press or the timeout defined in `CONFIG_APP_NTN_TIMER_TIMEOUT_MINUTES`.

## State Machine

![System overview](docs/images/ntn_state_machine.png)

## Configuration

Configure the UDP endpoint IP address and port in `prj.conf`:

```kconfig
CONFIG_APP_NTN_SERVER_ADDR=""
CONFIG_APP_NTN_SERVER_PORT=
```

## Build Instructions

### NTN Skylo (Monogoto SIM) - nRF9151 DK

```shell
west build app -b nrf9151dk/nrf9151/ns -- -DEXTRA_CONF_FILE=overlay-ntn-skylo-monogoto.conf
```

### NTN Skylo (Deutsche Telekom SIM) - nRF9151 DK

```shell
west build app -b nrf9151dk/nrf9151/ns -- -DEXTRA_CONF_FILE=overlay-ntn-skylo-dt.conf
```

### NTN Skylo (Soracom SIM) - nRF9151 DK

```shell
west build app -b nrf9151dk/nrf9151/ns -- -DEXTRA_CONF_FILE=overlay-ntn-skylo-soracom.conf
```

### NTN Amarisoft Callbox - nRF9151 DK

```shell
west build app -b nrf9151dk/nrf9151/ns -- -DEXTRA_CONF_FILE=overlay-ntn-amari.conf
```

## Flash and Run

By default, the device reports temperature data to the UDP endpoint. The last four digits of the device IMEI are used for identification.

```text
Device: *xxxx, temp: xx
```

If `CONFIG_APP_NTN_SEND_GNSS_DATA` is enabled, GNSS data is sent in addition to temperature data:

```text
Device: *xxxx, temp: xx, lat=xx.xx, lon=xx.xx, alt=xx.xx, time=xxxx-xx-xx xx:xx:xx
```

## Application Design

### Library Usage

The application uses the [NTN library](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/libraries/modem/ntn.html) to manage location updates. The library uses the `AT%LOCATION` command to subscribe to location update requests from the modem and to provide the necessary location data.

Additionally, the application uses [PDN_LTE_LC](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/libraries/modem/lte_lc.html#pdn_management) and `lte_lc_cellular_profile_configure` to manage PDN contexts and cellular profiles.

### PDN Event Handling

The application handles `LTE_LC_EVT_PDN_SUSPENDED` and `LTE_LC_EVT_PDN_RESUMED` events. These events indicate when PDNs are active but temporarily unusable. This mechanism supports context retention during temporary coverage loss, which is critical for efficient NTN operation.

## NTN Considerations

Operating in NTN networks presents challenges due to limited data rates and high latency. This requires optimized cellular link usage and connection setup strategies.

### Connection Management

- **Network Registration**: Functional Mode 45 retains network registration. The modem enters offline mode but keeps the network registration tied to the active PDN profile.
- **Context Retention**: Switching between operating modes avoids re-attaching, conserving cost, power, and time.
- **ePCO**: Extended Protocol Configuration Options (ePCO) must be disabled for Skylo networks (`LTE_LC_PDN_LEGACY_PCO=y`).

### Band and Channel Locking

To optimize the search for available cells, band and channel locking can be configured via Kconfig.

> **Note:** The NTN modem searches NTN specific bands (e.g., B23, B255, B256) only if band lock or channel lock is disabled.

Available Kconfig options:

- `CONFIG_APP_NTN_BANDLOCK_ENABLE`
- `CONFIG_APP_NTN_BANDLOCK`
- `CONFIG_APP_NTN_CHANNEL_SELECT_ENABLE`
- `CONFIG_APP_NTN_CHANNEL_SELECT`

## Location Management

Proper location management is crucial for NTN operation as the modem requires accurate position data to synchronize with satellites.

### Location Updates

The application receives notifications from the modem via the [NTN library](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/libraries/modem/ntn.html) regarding the required location accuracy. The modem triggers a notification only when the requested accuracy changes. The application is responsible for keeping the location up to date according to these requirements.

Location is set using the NTN library function:

```c
int ntn_location_set(double latitude, double longitude, float altitude, uint32_t validity);
```

### Validity and Update Interval

The `validity` parameter informs the modem how long the provided location remains valid.

- **Update Interval**: Must be calculated based on the device's maximum velocity and the modem's requested accuracy.
  - *Example*: A device moving at 120 km/h (~33 m/s) with a requested accuracy of 200 meters requires updates at least every 6 seconds.
- **Validity Duration**: The validity time must be sufficient to cover the period until the next update.

If the validity expires without an update, the modem considers the location invalid and may cease network signaling. This causes the device to transition to IDLE mode and drop the cell, potentially triggering a re-connection cycle.

### Modem Accuracy Requests

The modem determines the `requested_accuracy` based on its current state (e.g., IDLE vs. CONNECTED).

- **IDLE Mode**: Lower accuracy may be sufficient.
- **Transition to CONNECTED**: Higher accuracy is typically required to initiate a connection.
