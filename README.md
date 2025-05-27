# Asset Tracker Template - LEO usecase

This sample demonstrates an Asset Tracker Template variant for LEO/NTN use cases on Nordic nRF91 devices, including NTN shell controls, GNSS positioning, SGP4 pass prediction, and optional Onomondo SoftSIM and Memfault integration. On boot, the app starts a GNSS search, collects location and date/time from the fix, expects TLE data to be provided as input, then runs SGP4 to predict the next satellite pass and waits for that pass window.

## Instructions
Configure UDP endpoint in prj.conf:
```shell
CONFIG_APP_NTN_SERVER_ADDR=""
CONFIG_APP_NTN_SERVER_PORT=
```

#### NTN Sateliot - nRF9151 DK - Build Command

```shell
west build app -b nrf9151dk/nrf9151/ns -- -DEXTRA_CONF_FILE=overlay-ntn-sateliot.conf
```

## Shell commands available

NTN shell commands under `att_ntn`:

- `att_ntn ntn_trigger`: trigger the NTN state manually.
- `att_ntn gnss_trigger`: enter the GNSS state manually.
- `att_ntn sgp4_trigger`: run SGP4 manually using the currently cached GNSS fix and TLE.
- `att_ntn set_time_of_pass "<YYYY-MM-DD-HH:MM:SS>"`: set the next time of pass manually.
- `att_ntn set_datetime "<YYYY-MM-DD-HH:MM:SS>"`: set the modem date and time manually.
- `att_ntn set_gnss_location <lat> <lon> <alt_m>`: inject a GNSS fix directly without running the GNSS state.
- `att_ntn set_tle "<name>" "<line1>" "<line2>"`: provision TLE data directly without fetching it from the network.

Note: Running SGP4 requires valid datetime and GNSS location data, both provided either manually or through a GNSS search, plus TLE data to be provided manually.

```shell
att_ntn ntn_trigger
att_ntn gnss_trigger
att_ntn sgp4_trigger
att_ntn set_time_of_pass "2025-10-07-14:30:00"
att_ntn set_datetime "2025-10-07-14:30:00"
att_ntn set_gnss_location 57.7089 11.9746 15.0
att_ntn set_tle "SIOT1" \
  "1 12345U 24001A   26077.50000000  .00000000  00000-0  00000-0 0  9991" \
  "2 12345  86.4000 180.0000 0001000   0.0000 180.0000 14.20000000    01"
```

<br>
<br>

## \**Advanced: Onomondo SoftSIM + Memfault + TLE from Cloud **

![Experimental](https://img.shields.io/badge/status-experimental-orange)

### Description
Device boots, connects to nRFCloud via TN SoftSIM, downloads TLE data from shadow (or via HTTP client to if CONFIG_TLE_VIA_HTTP). Then goes to GNSS state and gets fix. After having collected TLE and GNSS, it runs SGP4 and computes next pass. (Note: only one satellite in the shadow limitation). After computing next pass, it schedules gnss_timer and ntn_timer. Gnns_timer to wake up 5 minutes before pass to update location. Ntn_timer to switch to ntn mode and scan for cell. If attach complete, it sends data to Thingy World (https://world.thingy.rocks/). 

### Instructions
Follow Onomondo softsim ncs installation: https://github.com/onomondo/nrf-softsim

Then clone this branch into samples folder, as in:
~/ncs-softsim/modules/lib/onomondo-softsim/samples/asset-tracker-template

Copy revision of app/west.yml into root ~/ncs-softsim/modules/lib/onomondo-softsim/west.yml

And then west update pointing to onomondo manifest.

Add sysbuild.conf as in app/sysbuild.conf with one line:
```shell
SB_CONFIG_SOFTSIM_BUNDLE_TEMPLATE_HEX=y
```

Add overlay-softsim.conf to CmakeLists.txt:
```shell
list(APPEND OVERLAY_CONFIG "$ENV{ZEPHYR_BASE}/../modules/lib/onomondo-softsim/samples/asset-tracker-template/app/overlay-softsim.conf")
```

Configure Memfault project key in overlay-memfault.conf:
```shell
CONFIG_MEMFAULT_NCS_PROJECT_KEY=""
```

Configure softsim static profile in overlay-softsim.conf:
```shell
CONFIG_SOFTSIM_STATIC_PROFILE=""
```

Note: Need to patch the shadow with TLE data. See tle_scripts/update_tle.py.

## NTN Sateliot - nRF9151 DK

```shell
west build app -b nrf9151dk/nrf9151/ns -- -DEXTRA_CONF_FILE="overlay-ntn-sateliot.conf;overlay-softsim-app.conf;overlay-memfault.conf;overlay-upload-modem-traces-to-memfault.conf"
```
