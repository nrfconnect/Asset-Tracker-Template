# PPR core image (nRF9251)

Minimal application for the nRF9251 Peripheral Processor (PPR). It logs
`hello world from <board target>` at a fixed interval. The log output is forwarded
over IPC (icmsg-me) to the application core and printed on the application
core's log backends with a `cpuppr/` prefix. The PPR has no UART console.

## Enabling

The image is built, launched and wired up for log forwarding by the sysbuild option
`SB_CONFIG_LAUNCH_PPR`:

```shell
west build -p -b nrf9251dk/nrf9251/cpuapp app -- -DSB_CONFIG_LAUNCH_PPR=y
```

Set the interval (in seconds, default 5) for the `ppr` image:

```shell
west build -p -b nrf9251dk/nrf9251/cpuapp app -- -DSB_CONFIG_LAUNCH_PPR=y \
  -Dppr_CONFIG_PPR_HELLO_INTERVAL_SECONDS=10
```

## Files

* `prj.conf`, `app.overlay`: PPR side (IPC log backend, icmsg-me follower).
* `cpuapp_ipc_log.conf`, `cpuapp_ipc_log.overlay`: application core side (PPR
  launch, IPC log link, icmsg-me initiator). Added to the application image by
  `app/sysbuild.cmake`.

The PPR image is flashed to `cpuppr_code_partition` and is not part of the
MCUboot-signed application image, so FOTA does not update it.
