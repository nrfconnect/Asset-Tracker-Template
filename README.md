# Asset Tracker Template - LEO usecase

Simple display of GNSS + NTN usecase:

    a. Upon boot, GNSS cold start to get UTC time.

    b. Provision ATT with time of pass via shell 'att_ntn_set_time "2025-10-07-14:30:00"'.

    c. Go sleep.

    d. Obtain GNSS at t-300sec

    e. Go sleep.

    f. Enable LTE at t-20sec

    e. Once connected, send cloud data to Thingy World (https://world.thingy.rocks/)

Configure Thingy World endpoint in overlay file:
```shell
CONFIG_APP_NTN_SERVER_ADDR=""
CONFIG_APP_NTN_SERVER_PORT=
```

Select following Kconfig to use world.thingy.rocks data format:
```shell
CONFIG_APP_NTN_THINGY_ROCKS_ENDPOINT=y
```

## NTN Sateliot - nRF9151 DK

```shell
west build -b nrf9151dk/nrf9151/ns app -- -DEXTRA_CONF_FILE=overlay-ntn-sateliot.conf
```


## NTN OQtech - nRF9151 DK

```shell
west build -b nrf9151dk/nrf9151/ns app -- -DEXTRA_CONF_FILE=overlay-ntn-oqtech.conf
```


## Flash and run

App should report GNSS data to UDP endpoint, upon successfull connection.
