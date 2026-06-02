# Network module

The Network module manages cellular connectivity for nRF91 Series devices with terrestrial (TN) and NTN GEO fallback. It handles connection states, dual cellular profiles (terrestrial on profile 0, NTN on profile 1), GNSS location preconditioning for NTN search, and power saving (eDRX, PSM). It uses [LTE Link Control](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/libraries/modem/lte_lc.html) and the NTN library. The module implements a state machine with Zephyr's [State Machine Framework](https://docs.zephyrproject.org/latest/services/smf/index.html).

The module starts idle and stays idle until the [Main module](main.md) publishes `NETWORK_CONNECT_TN`. Main owns the connection lifecycle and handles TN → NTN fallback on `NETWORK_TN_SEARCH_FAILED`.

The module tracks both the PDN context and the network registration status. Losing registration (for example `+CEREG: 4` when moving out of coverage) is reported as `NETWORK_DISCONNECTED` even while the PDN context is still active, so the cloud session can be paused before data transactions start failing. If registration is regained while the PDN survived, the module re-asserts the current connectivity (`NETWORK_CONNECTED_TN` or `NETWORK_CONNECTED_NTN`) so a paused cloud session can resume.

## Architecture

### State diagram

```mermaid
stateDiagram-v2
    direction TB

    state Running {
        [*] --> Disconnected
        Disconnected --> ConnectedTN: NETWORK_CONNECTED_TN
        Disconnected --> ConnectedNTN: NETWORK_CONNECTED_NTN
        ConnectedTN --> Disconnecting: NETWORK_DISCONNECT
        ConnectedNTN --> Disconnecting: NETWORK_DISCONNECT
        Disconnecting --> DisconnectedIdle: NETWORK_DISCONNECTED
    }

    state Disconnected {
        [*] --> Idle
        Idle --> TnSearching: NETWORK_CONNECT_TN
        Idle --> NtnSearching: NETWORK_CONNECT_NTN
        TnSearching --> Idle: NETWORK_TN_SEARCH_FAILED
        state NtnSearching {
            [*] --> CheckLocation
            CheckLocation --> AwaitingLocation: LOCATION_NEEDED
            CheckLocation --> CellSearch: LOCATION_VALID
            AwaitingLocation --> CellSearch: NETWORK_GNSS_LOCATION
            NtnSearching --> Idle: NETWORK_GNSS_LOCATION_FAILED
            CellSearch --> Idle: NO_SUITABLE_CELL_NTN
        }
    }

    state Connected {
        ConnectedTN
        ConnectedNTN
    }
```

The Main module orchestrates fallback: on `NETWORK_TN_SEARCH_FAILED` it publishes `NETWORK_CONNECT_NTN`. On `NETWORK_GNSS_LOCATION_REQ` it triggers a GNSS-only location search and returns the fix via `NETWORK_GNSS_LOCATION`.

### SMF state names

| Diagram state | SMF symbol |
|---------------|------------|
| `STATE_DISCONNECTED_IDLE` | `STATE_DISCONNECTED_IDLE` |
| `STATE_DISCONNECTED_TN_SEARCHING` | `STATE_DISCONNECTED_TN_SEARCHING` |
| `STATE_DISCONNECTED_NTN_SEARCHING` | `STATE_DISCONNECTED_NTN_SEARCHING` |
| `STATE_DISCONNECTED_NTN_CHECK_LOCATION` | `STATE_DISCONNECTED_NTN_CHECK_LOCATION` |
| `STATE_DISCONNECTED_NTN_AWAITING_LOCATION` | `STATE_DISCONNECTED_NTN_AWAITING_LOCATION` |
| `STATE_DISCONNECTED_NTN_CELL_SEARCH` | `STATE_DISCONNECTED_NTN_CELL_SEARCH` |
| `STATE_CONNECTED_TN` / `STATE_CONNECTED_NTN` | `STATE_CONNECTED_TN` / `STATE_CONNECTED_NTN` |
| `STATE_DISCONNECTING` | `STATE_DISCONNECTING` |

Internal events `LOCATION_VALID` and `LOCATION_NEEDED` are raised on `priv_network_chan` from `STATE_DISCONNECTED_NTN_CHECK_LOCATION` entry; transitions run in that state's **run** handler. `NETWORK_GNSS_LOCATION_FAILED` is handled on the `STATE_DISCONNECTED_NTN_SEARCHING` parent; `NO_SUITABLE_CELL_NTN` (`NETWORK_NTN_SEARCH_FAILED`) is handled in `STATE_DISCONNECTED_NTN_CELL_SEARCH` only.

## Messages

The network module communicates through the zbus channel `network_chan`. Messages are defined in `network.h`.

### Input messages

- **NETWORK_CONNECT_TN**: Start terrestrial search (LTE-M + NB-IoT + GPS).
- **NETWORK_CONNECT_NTN**: Start NTN search (location check, optional GNSS, then NTN NB-IoT). With `.fresh_location` set, a new GNSS fix is acquired before the cell search even when the cached fix is still valid, so the fix can double as a location sample.
- **NETWORK_DISCONNECT**: Disconnect.
- **NETWORK_SEARCH_STOP**: Stop search (`lte_lc_offline()`).
- **NETWORK_GNSS_LOCATION**: Provide lat/lon/alt (from Main / Location module).
- **NETWORK_GNSS_LOCATION_FAILED**: GNSS fix failed or timed out.

A `NETWORK_CONNECT_TN` or `NETWORK_CONNECT_NTN` received while already connected re-asserts the current connectivity instead of starting a new search.

### Output messages

- **NETWORK_DISCONNECTED**: PDN context deactivated, or network registration lost while the PDN is still active.
- **NETWORK_CONNECTED_TN** / **NETWORK_CONNECTED_NTN**: Connected over terrestrial or NTN.
- **NETWORK_TN_SEARCH_FAILED** / **NETWORK_NTN_SEARCH_FAILED**: Search failed (Main may fall back TN → NTN).
- **NETWORK_GNSS_LOCATION_REQ**: Module needs a GNSS fix before NTN cell search.
- **NETWORK_UICC_FAILURE**, **NETWORK_PSM_PARAMS**, **NETWORK_EDRX_PARAMS**

## Build

Requires `CONFIG_NTN`, `CONFIG_LTE_LC_CELLULAR_PROFILE_MODULE`, and NTN modem firmware (`mfw_nrf9151-ntn`) on hardware targets.

```bash
tm33 west build -b thingy91x/nrf9151/ns app
```

## Unit tests

```bash
cd tests/module/network
att_run_32 west build -b native_sim .
att_run_32 ./build/network/zephyr/zephyr.exe
```
