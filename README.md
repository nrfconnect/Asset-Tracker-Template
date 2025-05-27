# Asset Tracker Template - TN NTN use case

Note: you need device provisioned with nRFCloud. If not already, follow docs on ATT main or Quickstart app.

The main idea is to use TN to establish DTLS with the cloud, and then have NTN profit by the same connection.
So to avoid handshake via NTN (which has proven to be feasible but to be used as fallback).

## NTN Skylo (Deutsche Telekom) - nRF9151 DK

```shell
west build -b nrf9151dk/nrf9151/ns app -- -DEXTRA_CONF_FILE=overlay-ntn-skylo-dt.conf
```

## NTN Skylo (Soracom) - nRF9151 DK

```shell
west build -b nrf9151dk/nrf9151/ns app -- -DEXTRA_CONF_FILE=overlay-ntn-skylo-soracom.conf
```

## NTN Amarisoft Callbox - nRF9151 DK

```shell
west build -b nrf9151dk/nrf9151/ns app -- -DEXTRA_CONF_FILE=overlay-ntn-amari.conf
```

## State machine
![System overview](docs/images/ntn_state_machine.png)
