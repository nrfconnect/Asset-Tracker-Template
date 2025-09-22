# Asset Tracker Template - TN NTN nRFCloud usecase

Note: you need device provisioned with nRFCloud. If not already, follow docs on ATT main or Quickstart app.

The main idea is to use TN to establish DTLS with the cloud, and then have NTN profit by the same connection.
So to avoid handshake via NTN (which has proven to be feasible but to be used as fallback).

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

## State machine
![System overview](docs/images/ntn_state_machine.png)

## LED patterns

/* Blue pattern for Network connected */
/* Green pattern for cloud connection */
/* Yellow yellow for TN network search */
/* Orange pattern during NTN network search */
/* Purple pattern during GNNS search */
/* White pattern when idle */
