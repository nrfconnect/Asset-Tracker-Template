# Asset Tracker Template - LEO usecase

Follow Onomondo softsim ncs installation: https://github.com/onomondo/nrf-softsim

Then clone this branch into samples folder, as in:
~/ncs-softsim/modules/lib/onomondo-softsim/samples/asset-tracker-template

Copy content of app/overlay-softsim.conf into root ~/ncs-softsim/modules/lib/onomondo-softsim/overlay-softsim.conf


Configure Thingy World endpoint in prj.conf:
```shell
CONFIG_APP_NTN_SERVER_ADDR=""
CONFIG_APP_NTN_SERVER_PORT=
```

Configure Memfault project key in overlay-ntn-sateliot.conf:
```shell
CONFIG_MEMFAULT_NCS_PROJECT_KEY=""
```

Configure softsim static profile in ~/ncs-softsim/modules/lib/onomondo-softsim/overlay-softsim.conf:
```shell
CONFIG_SOFTSIM_STATIC_PROFILE=""
```

Note: Need to patch the shadow with TLE data. See tle_scripts/update_tle.py.

Device boots, connects to nRFCloud via TN SoftSIM, downloads TLE data from shadow. Then goes to GNSS state and gets fix. After having collected TLE and GNSS, it runs SGP4 and computes next pass. (Note: only one satellite in the shadow). After computing next pass, it schedules gnss_timer and ntn_timer. Gnns_timer to wake up 5 minutes before pass to update location. Ntn_timer to switch to ntn mode and scan for cell. If attach complete, it sends data to Thingy World (https://world.thingy.rocks/). 

## NTN Sateliot - nRF9151 DK

```shell
west build app -b nrf9151dk/nrf9151/ns -- -DEXTRA_CONF_FILE=overlay-ntn-sateliot.conf
```
