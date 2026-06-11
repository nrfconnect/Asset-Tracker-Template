# UART Power Control module

The UART Power Control module suspends and resumes UART peripherals based on VBUS (USB) presence on devices with an nPM1300 PMIC, such as the Thingy:91 X. This reduces power consumption when the device runs on battery.

When the option is enabled, the module:

- Subscribes to VBUS detected and removed events from the nPM1300 charger.
- Suspends `uart0` and `uart1` (and the modem trace UART, if configured) when VBUS is removed.
- Resumes the same UARTs when VBUS is detected.

## Architecture

The module has no state machine and exposes no zbus channel. It runs a single low-priority thread (`uart_power_control_thread`) that:

1. Waits on a semaphore released from the `NRF_MODEM_LIB_ON_INIT` callback. This sequencing prevents UART contention while the modem library initializes the trace UART.
2. Registers a callback with the nPM1300 MFD driver for the `NPM13XX_EVENT_VBUS_DETECTED` and `NPM13XX_EVENT_VBUS_REMOVED` events.
3. Reads the current VBUS state once and applies the matching UART action.

Subsequent VBUS transitions are handled directly in the callback, which calls `pm_device_action_run()` with `PM_DEVICE_ACTION_SUSPEND` or `PM_DEVICE_ACTION_RESUME` on each UART device.

## Configuration

- **CONFIG_APP_UART_POWER_CONTROL:**
  Enables the module. Default `n`.

- **CONFIG_APP_UART_POWER_CONTROL_THREAD_STACK_SIZE:**
  Stack size for the initialization thread. Default `512` bytes.

- **CONFIG_APP_UART_POWER_CONTROL_LOG_LEVEL:**
  Log level for the module.

See the `Kconfig.uart_power_control` file in the module directory for all available options.

## Devicetree

The module requires the following nodes to be present and enabled in devicetree:

- `npm1300_charger` — nPM1300 charger node, used to read VBUS status.
- `pmic_main` — nPM1300 MFD parent node, used to register the VBUS event callback.
- `uart0`, `uart1` — UART devices that are suspended and resumed.

## Notes

- The trace UART is only touched when `CONFIG_NRF_MODEM_LIB_TRACE_BACKEND_UART` is enabled.
- After suspend, the shell on the affected UART is unreachable until VBUS is reconnected.

## Related

- [Power module](power.md) — battery monitoring and fuel gauge handling.
- [Achieving low power](../common/low_power.md) — context on when to use this module.
