# Asset Tracker Template

This is the firmware that is programmed on the nRF9151 of the Thingy:91 X in factory.
It's goal is to showcase the device's functionality and to spur interest into trying out the device.
[nRF Cloud CoAP](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/libraries/networking/nrf_cloud_coap.html) is used as the cloud solution.

The architecture is based on [ZBUS](https://docs.zephyrproject.org/latest/services/zbus/index.html).
Rather than having a `main.c` file to tie the application together, 12 modules communicate via ZBUS channels.
This enables us to keep the modules relatively small and reusable by separation of concerns.

Cloud to device communication is done with runtime settings. Device to cloud communication uses CBOR encoded LwM2M objects. Each module encodes its own data using ZCBOR. The format is specified in CDDL files and encoding functions are automatically generated. The encoded data is sent to the payload channel, which takes care of forwarding it to cloud.

## Trigger module
* beating heart of the application: send triggers to control other modules
* handles the different states the application is in

## Transport module
* manage connection to nRF Cloud CoAP, notify about cloud connection status
* forward payloads to cloud

## Network module
* wrap [connection manager](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/connectivity/networking/conn_mgr/main.html) subsystem and notify about network events

## FOTA module
* FOTA handling using nRF Cloud CoAP, notifies about FOTA status

## App module
* manages runtime settings
* wrap [`date_time`](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/libraries/others/date_time.html) library, notify when time is available

## Memfault module
* upload runtime stats and coredumps to [Memfault](https://memfault.com/) for easier debugging

## Battery module
* wrap [`fuel_gauge`](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/hardware/peripherals/fuel_gauge.html) subsystem and publish battery status as payload

## Button module
* notify other modules about button presses
* publish button message as payload

## Environment module
* wrap [`bm68x_iaq`](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/drivers/bme68x_iaq.html) driver and publish air quality, temperature, humidity data as payload

## LED module
* monitor various events and display sophisticated LED patterns

## Location module
* wrap [`location`](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/libraries/modem/location.html) library, gather and send location data
* note: data is sent directly using the library rather than being repackaged in the module

## Shell module
* test related shell functions
* turn off UART for power saving when leaving polling mode

# Overview of modules reading/writing ZBUS channels
Note: the ERROR channel and channels only used internally in modules were omitted.

| CHANNEL      | battery | environment | location | network | trigger | app | button | fota | led | memfault | shell | cloud     |
|--------------|---------|-------------|----------|---------|---------|-----|--------|------|-----|----------|-------|-----------|
| CLOUD        |         |             | r        |         | r       | r   |        | r    |     | r        |       | w         |
| CONFIG       |         |             | r        |         | r       | w   |        |      |     |          |       |           |
| LOCATION     |         |             | w        |         | r       |     |        |      | r   |          |       |           |
| BUTTON       |         |             |          |         | r       |     | w      |      |     |          | w     |           |
| FOTA         |         |             |          |         | r       |     |        | w    |     |          |       |           |
| TRIGGER      | r       | r           | r        | r       | w       |     |        |      |     |          |       |           |
| TRIGGER_MODE |         |             |          |         | w       |     |        |      | r   |          |       |           |
| NETWORK      | r       |             | r        | w       |         |     |        |      | r   |          |       | r         |
| PAYLOAD      | w       | w           |          | w       |         |     | w      |      |     |          |       | r         |
| TIME         | r       | r           |          | r       |         | w   |        |      | r   |          |       |           |
