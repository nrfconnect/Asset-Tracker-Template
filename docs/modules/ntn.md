# NTN module

The NTN module adds an Iridium non-terrestrial network (NTN) fallback to the Asset Tracker Template. When no terrestrial network (TN) is available, the module suspends the Network module, switches the modem to Iridium NTN (TDD NB-IoT), and periodically sends the device's GNSS location to nRF Cloud over satellite. It returns to the terrestrial network on a configurable interval.

The module is an example located in `examples/modules/ntn`. It is disabled by default and does not change the behavior of the template unless you enable it with `examples/modules/ntn/overlay-ntn.conf`.

The same Deutsche Telekom SIM card and APN are used for the terrestrial network (LTE-M/NB-IoT) and for Iridium NTN.

## Requirements

- nRF9151 with modem firmware supporting Iridium NTN.
- A Deutsche Telekom SIM card provisioned for Iridium NTN.
- An antenna setup suitable for both GNSS and NTN.

## Building

Run the following command from the root of the repository:

```shell
west build -p -b nrf9151dk/nrf9151/ns app -- -DEXTRA_CONF_FILE="$(pwd)/examples/modules/ntn/overlay-ntn.conf"
```

## How it works

The modem is configured with two cellular profiles at boot: profile 0 for the terrestrial network and profile 1 for NTN. Both use the physical SIM card. With two profiles, the modem can be put into offline mode while keeping its registration context (`LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG`), which is how the module switches between TN, GNSS, and NTN.

1. **Terrestrial network.** The template runs as usual. If the Network module does not connect within `CONFIG_APP_NTN_TN_FALLBACK_TIMEOUT_SECONDS`, or reports `NETWORK_SEARCH_DONE` (with `CONFIG_APP_NTN_FALLBACK_ON_SEARCH_DONE`), the module falls back to NTN.
1. **Suspend.** The module sends `NETWORK_SUSPEND`. The Network module takes the modem offline, ignores link events, and reports `NETWORK_SUSPENDED` followed by `NETWORK_DISCONNECTED`. The Cloud module pauses the nRF Cloud CoAP session, which saves the DTLS Connection ID session in the modem.
1. **NTN idle.** Between uplinks, the modem is left in GNSS-only mode, so the Location module can get fixes. The module caches the latest GNSS location from `LOCATION_GNSS_DATA`.
1. **NTN uplink.** Every `CONFIG_APP_NTN_UPLINK_INTERVAL_SECONDS`, the module provides the location to the modem with `ntn_location_set()`, activates NTN, and waits for registration. **PDN resumed** or **PDN activated** triggers a UDP **service request** once the modem is registered; the CoAP send waits for **registered**, **PDN up**, and **RRC connected** (on the PDN-resume path, RRC must come **after** resume). The module resumes the paused CoAP session, updates enabled shadow info sections, then sends the location as a confirmable message, pauses again, and takes the modem offline. NTN builds extend CoAP ACK timeouts in `overlay-ntn.conf`. If the location is older than `CONFIG_APP_NTN_LOCATION_VALIDITY_SECONDS`, the module triggers a new GNSS search first.
1. **Return to TN.** Every `CONFIG_APP_NTN_TN_RETRY_INTERVAL_MINUTES`, the module restores the terrestrial system mode and sends `NETWORK_RESUME`. The Network module searches for a network as usual, and the Cloud module resumes the CoAP session when connected, or connects again if the resume fails.

GNSS and NTN cannot be active at the same time, so the module time-multiplexes them.

## Messages

The NTN module communicates through the zbus channel `ntn_chan`. The messages are defined in `ntn.h`.

### Input messages

- **NTN_FALLBACK_START_REQUEST**: Fall back to NTN immediately.
- **NTN_FALLBACK_STOP_REQUEST**: Stop using NTN and return to the terrestrial network.
- **NTN_UPLINK_REQUEST**: Send the location over NTN as soon as possible.
- **NTN_LOCATION_SET**: Set the location used for NTN, for example when GNSS is not available. The location is carried in `struct ntn_location`.

### Output messages

- **NTN_FALLBACK_STARTED**: The module has suspended the Network module and is using NTN.
- **NTN_FALLBACK_STOPPED**: The module has returned control to the Network module.
- **NTN_UPLINK_SENT**: The location was sent to nRF Cloud over NTN.
- **NTN_UPLINK_FAILED**: Sending the location over NTN failed.

## Changes to other modules

The NTN module reuses the rest of the template. It relies on two small generic additions to other modules:

- **Network module**: `NETWORK_SUSPEND` and `NETWORK_RESUME` input messages, the `NETWORK_SUSPENDED` output message, and a suspended state. Only used when another module sends `NETWORK_SUSPEND`.
- **Cloud module**: pauses the nRF Cloud CoAP session when the network is lost or suspended and resumes it when reconnected, instead of doing a new DTLS handshake. If the resume fails, it connects to nRF Cloud again.

## Shell commands

With `CONFIG_APP_NTN_SHELL`, the following commands are available:

- `att_ntn start`: Fall back to NTN now.
- `att_ntn stop`: Return to the terrestrial network.
- `att_ntn send`: Send the location over NTN now.
- `att_ntn location <lat> <lon> <alt_m> [accuracy_m]`: Set the location manually.

## Configuration

- **CONFIG_APP_NTN**: Enable the module.
- **CONFIG_APP_NTN_LOG_LEVEL_DBG** / **CONFIG_NTN_LOG_LEVEL_DBG** / **CONFIG_LTE_LINK_CONTROL_LOG_LEVEL_DBG**: Debug logging for the NTN module (`ntn_module`), modem NTN library (`ntn`), and LTE Link Control (`lte_lc`). The first two are set in `overlay-ntn.conf`, the `lte_lc` one is commented out there.
- **CONFIG_APP_NTN_TN_FALLBACK_TIMEOUT_SECONDS**: Time without a terrestrial connection before falling back to NTN.
- **CONFIG_APP_NTN_FALLBACK_ON_SEARCH_DONE**: Fall back to NTN as soon as the modem has completed a full terrestrial network search.
- **CONFIG_APP_NTN_TN_RETRY_INTERVAL_MINUTES**: How often to try the terrestrial network again while using NTN.
- **CONFIG_APP_NTN_UPLINK_INTERVAL_SECONDS**: Location uplink interval over NTN.
- **CONFIG_APP_NTN_LOCATION_VALIDITY_SECONDS**: Maximum age of the location given to the modem. Set to 0 for no limit.
- **CONFIG_APP_NTN_CONNECTION_TIMEOUT_SECONDS**: Time allowed to register over NTN before giving up on an uplink.
- **CONFIG_APP_NTN_REGISTERED_TIMEOUT_SECONDS**: Time allowed after registration for RRC and PDN setup to complete.
- **CONFIG_APP_NTN_SERVICE_REQUEST_ADDR** / **CONFIG_APP_NTN_SERVICE_REQUEST_PORT**: Destination for the UDP service-request packet on PDN resume/activate (default 8.8.8.8:53).
- **CONFIG_APP_NTN_BANDLOCK**: NTN band lock (default 249, Iridium).
- **CONFIG_APP_NTN_CHANNEL_SELECT**: NTN channel EARFCN (default 196702).
- **CONFIG_APP_NTN_CLOUD_FULL_CONNECT**: Allow a full DTLS handshake over NTN if the paused session cannot be resumed. Disabled by default, as the handshake is expensive over satellite.

`overlay-ntn.conf` also disables PSM requests for the whole application, and limits CoAP retransmissions so that an uplink fits within `CONFIG_APP_NTN_MSG_PROCESSING_TIMEOUT_SECONDS`.

## Limitations

- Each NTN uplink refreshes the enabled device shadow info sections **before** the GNSS location CoAP send. On Iridium, `modem_info` often cannot read cell ID from `AT+CEREG?`; the module caches TAC/cell from `+CEREG` and `LTE_LC_EVT_CELL_UPDATE` and supplies that when encoding shadow data. Signal quality is read with `AT%SQP?` when the modem is activated (RSRP, RSRQ, SINR, RSSI, PCI, EARFCN). Environmental and other sensor data are still buffered by the Storage module and sent when the terrestrial network is available again.
- A location sent over NTN can also appear later in buffered storage data flushed over TN.
- Without a terrestrial network, there is no A-GNSS assistance. The first GNSS fix can take several minutes.
