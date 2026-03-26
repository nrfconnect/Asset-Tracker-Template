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
- `att_ntn sgp4_trigger [min_elevation_deg]`: run SGP4 manually using the currently cached GNSS fix and prediction data. Defaults to `40` degrees if no threshold is provided.
- `att_ntn idle_trigger`: force the NTN module back to the idle state manually.
- `att_ntn set_peak_offset <seconds>`: override the NTN activation offset relative to predicted peak time. Positive values trigger before peak, negative values after peak.
- `att_ntn set_time_of_pass "<YYYY-MM-DD-HH:MM:SS>"`: set the next time of pass manually.
- `att_ntn set_datetime "<YYYY-MM-DD-HH:MM:SS>"`: set the modem date and time manually.
- `att_ntn set_gnss_location <lat> <lon> <alt_m>`: inject a GNSS fix directly without running the GNSS state.
- `att_ntn set_tle "<name>" "<line1>" "<line2>"`: provision TLE data directly without fetching it from the network.
- `att_ntn set_sib32 "<SIBCONFIG: 32,...>"`: provision raw SIB32 prediction data directly from a modem notification string.

Note: Running SGP4 requires valid datetime and GNSS location data, both provided either manually or through a GNSS search, plus prediction data provided manually as either `TLE` or `SIB32`.

```shell
att_ntn ntn_trigger
att_ntn gnss_trigger
att_ntn sgp4_trigger
att_ntn sgp4_trigger 50
att_ntn idle_trigger
att_ntn set_peak_offset 45
att_ntn set_time_of_pass "2025-10-07-14:30:00"
att_ntn set_datetime "2025-10-07-14:30:00"
att_ntn set_gnss_location 57.7089 11.9746 15.0
att_ntn set_tle "SIOT1" \
  "1 12345U 24001A   26077.50000000  .00000000  00000-0  00000-0 0  9991" \
  "2 12345  86.4000 180.0000 0001000   0.0000 180.0000 14.20000000    01"
att_ntn set_sib32 "SIBCONFIG: 32,\"00000001\",2,1,1138123,1529334,1391197,758633,13719,2572629918,28139,-3,120679,,11,11,,,,3,1138188,1686202,1399132,2534648,10028,2572728485,19572,-3,121275,,11,11,,,"
```

Example log:
```shell
*** Booting Asset Tracker Template v1.3.0-dev-4e16c3b9110f ***
*** Using nRF Connect SDK v3.2.99-3653fec21b6a ***
*** Using Zephyr OS v4.2.99-fbabee3e8169 ***
[00:00:00.257,476] <dbg> ntn_module: state_running_entry: state_running_entry
[00:00:00.519,897] <inf> nrf_modem_lib_trace: Trace thread ready
[00:00:00.521,881] <inf> nrf_modem_lib_trace: Trace level override: 2
[00:00:01.023,498] <wrn> ntn_module: Provide SIB32 or TLE before running SGP4: att_ntn set_sib32 "<SIBCONFIG: 32,...>" or att_ntn set_tle "<name>" "<line1>" "<line2>"
[00:00:01.023,559] <dbg> ntn_module: state_idle_entry: state_idle_entry
uart:~$ att_ntn set_datetime "2026-02-12-09:38:29"
Setting manual date time to: 2026-02-12-09:38:29
[00:00:04.473,205] <dbg> ntn_module: state_idle_run: state_idle_run
[00:00:04.483,306] <inf> ntn_module: Applied manual date time: 2026-02-12-09:38:29
uart:~$ att_ntn set_gnss_location 63.43 10.39 40.0
Injected GNSS location: lat=63.430000 lon=10.390000 alt=40.00 m
[00:00:08.321,411] <dbg> ntn_module: state_idle_run: state_idle_run
[00:00:08.321,533] <inf> ntn_module: Stored manual GNSS location: lat=63.430000 lon=10.390000 alt=40.00
uart:~$ att_ntn set_sib32 "SIBCONFIG: 32,\"00000001\",2,1,1138123,1529334,139119
7,758633,13719,2572629918,28139,-3,120679,,11,11,,,,3,1138188,1686202,1399132,25
34648,10028,2572728485,19572,-3,121275,,11,11,,,"
Provisioned manual SIB32 prediction data
[00:00:12.449,829] <dbg> ntn_module: state_idle_run: state_idle_run
[00:00:12.451,232] <dbg> sgp4_pass_predict: epochStar2jd_satepoch: weekday: 3 unix_time_ms1: 1770889116977
[00:00:12.451,232] <dbg> sgp4_pass_predict: epochStar2jd_satepoch: unix_time_ms2: 1770629916977
[00:00:12.451,293] <dbg> sgp4_pass_predict: epochStar2jd_satepoch: unix_time_ms3: 1770595200000
[00:00:12.454,772] <dbg> sgp4_pass_predict: epochStar2jd_satepoch: weekday: 3 unix_time_ms1: 1770889116981
[00:00:12.454,772] <dbg> sgp4_pass_predict: epochStar2jd_satepoch: unix_time_ms2: 1770629916981
[00:00:12.454,833] <dbg> sgp4_pass_predict: epochStar2jd_satepoch: unix_time_ms3: 1770595200000
[00:00:12.458,007] <inf> ntn_module: Stored SIB32 prediction data; it will be preferred over TLE
uart:~$ att_ntn set_tle "SATELIOT_1" "1 60550U 24149CL  26041.62762187  .0000312
4  00000+0  27307-3 0  9999" "2 60550  97.6859 119.6343 0008059 130.5773 229.615
2 14.97467371 81125"
Provisioned manual TLE for: SATELIOT_1
[00:00:15.705,963] <dbg> ntn_module: state_idle_run: state_idle_run
[00:00:15.706,054] <inf> ntn_module: Stored manual TLE 1/1 for SATELIOT_1
uart:~$ att_ntn set_tle "SATELIOT_3" "1 60552U 24149CN  26041.67052983  .0000257
8  00000+0  22594-3 0  9997" "2 60552  97.6924 120.3514 0005931 143.3246 216.838
3 14.97526427 81146"
Provisioned manual TLE for: SATELIOT_3
[00:00:19.290,710] <dbg> ntn_module: state_idle_run: state_idle_run
[00:00:19.290,802] <inf> ntn_module: Stored manual TLE 2/2 for SATELIOT_3
uart:~$ att_ntn sgp4_trigger
Triggering SGP4 manually
[00:00:22.889,862] <dbg> ntn_module: state_idle_run: state_idle_run
[00:00:22.889,923] <dbg> ntn_module: state_sgp4_entry: state_sgp4_entry
[00:00:22.891,784] <dbg> ntn_module: log_stack_usage: Stack before_sgp4: unused=14120 used~=2264/16384
[00:00:22.893,096] <dbg> sgp4_pass_predict: epochStar2jd_satepoch: weekday: 3 unix_time_ms1: 1770889127419
[00:00:22.893,127] <dbg> sgp4_pass_predict: epochStar2jd_satepoch: unix_time_ms2: 1770629927419
[00:00:22.893,188] <dbg> sgp4_pass_predict: epochStar2jd_satepoch: unix_time_ms3: 1770595200000
[00:00:22.896,636] <dbg> sgp4_pass_predict: epochStar2jd_satepoch: weekday: 3 unix_time_ms1: 1770889127423
[00:00:22.896,667] <dbg> sgp4_pass_predict: epochStar2jd_satepoch: unix_time_ms2: 1770629927423
[00:00:22.896,728] <dbg> sgp4_pass_predict: epochStar2jd_satepoch: unix_time_ms3: 1770595200000
[00:00:22.899,871] <inf> ntn_module: Using SIB32 data to compute next pass
[00:00:22.900,115] <dbg> sgp4_pass_predict: sat_data_calculate_next_pass: jd_epoch: 2461081.896748
[00:00:23.043,762] <dbg> sgp4_pass_predict: sat_data_calculate_next_pass: jd_epoch: 2461081.903646
[00:00:23.121,093] <inf> ntn_module: Selected satellite index 1 of 2
[00:00:23.121,551] <inf> ntn_module: Next pass: SIB32
[00:00:23.121,582] <inf> ntn_module: Start: 2026-02-12 10:17:47 UTC
[00:00:23.121,612] <inf> ntn_module: End: 2026-02-12 10:19:47 UTC
[00:00:23.121,673] <inf> ntn_module: Max elevation: 44.49 degrees at 2026-02-12 10:18:47 UTC
[00:00:23.131,835] <inf> ntn_module: Current time: 1770889127, Pass time: 1770891527
[00:00:23.131,866] <inf> ntn_module: Seconds until pass: 2400
[00:00:23.131,958] <inf> ntn_module: GNSS timer set to wake up in 2100 seconds
[00:00:23.131,958] <inf> ntn_module: NTN timer set to wake up in 2380 seconds
[00:00:23.132,019] <dbg> ntn_module: state_sgp4_exit: state_sgp4_exit
[00:00:23.132,049] <dbg> ntn_module: state_idle_entry: state_idle_entry
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
