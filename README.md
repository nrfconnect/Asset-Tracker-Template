# Asset Tracker Template - GNSS NTN usecase

Configure UDP endpoint in overlay file:
```shell
CONFIG_APP_NTN_SERVER_ADDR=""
CONFIG_APP_NTN_SERVER_PORT=
```

## NTN Skylo (DTAG) - t91x

```shell
west build -b thingy91x/nrf9151/ns app -- -DEXTRA_CONF_FILE=overlay-ntn-skylo-DT.conf
```

## NTN Skylo (Soracom) - t91x

```shell
west build -b thingy91x/nrf9151/ns app -- -DEXTRA_CONF_FILE=overlay-ntn-skylo-soracom.conf
```

## NTN amarisoft - nrf9151dk

```shell
west build -b nrf9151dk/nrf9151/ns app -- -DEXTRA_CONF_FILE=overlay-ntn-amari.conf
```

## Flash and run

App should report GNSS data to UDP endpoint:
```shell
GNSS: lat=xx.xx, lon=xx.xx, alt=xx.xx, time=xxxx-xx-xx xx:xx:xx
```

