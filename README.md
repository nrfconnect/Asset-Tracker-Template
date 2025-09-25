# Asset Tracker Template - LEO usecase

Simple display of GNSS + NTN usecase:

    a. Provision ATT with time of pass(es) via CONFIG_APP_NTN_LEO_TIME_OF_PASS.

    b. Upon boot, GNSS cold start to get UTC time.

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

## NTN Sateliot - t91x

```shell
west build -b thingy91x/nrf9151/ns app -- -DEXTRA_CONF_FILE=overlay-ntn-sateliot.conf
```

## NTN amarisoft - nrf9151dk

```shell
west build -b nrf9151dk/nrf9151/ns app -- -DEXTRA_CONF_FILE=overlay-ntn-amari.conf
```

## Flash and run

App should report GNSS data to Thingy World endpoint:
```shell
GNSS: lat=xx.xx, lon=xx.xx, alt=xx.xx, time=xxxx-xx-xx xx:xx:xx
```

