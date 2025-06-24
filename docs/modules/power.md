# Power module

The power module manages power-related functionality for devices with nPM1300, like the Thingy:91 X, including the following:

- Monitoring battery voltage and calculating remaining battery percentage.
- Handling VBUS (USB) connect and disconnect events to enable or disable UART peripherals.
- Publishing battery percentage updates via zbus messages.

## Messages

The Power module defines and communicates on the `POWER_CHAN` channel.

### Input Messages

- **POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST:**
  Requests a battery percentage sample.

### Output Messages

- **POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE:**
  Contains the calculated battery percentage.

The power message structure is defined in `power.h`:

```c
struct power_msg {
	enum power_msg_type type;

	/** Contains the current charge of the battery in percentage. */
	double percentage;
};
```

## Configurations

The following Kconfig options control this module’s behavior:

- **CONFIG_APP_POWER:**
  Enables the Power module.

- **CONFIG_APP_POWER_DISABLE_UART_ON_VBUS_REMOVED:**
  If enabled, suspends UART devices when VBUS is removed.

- **CONFIG_APP_POWER_THREAD_STACK_SIZE:**
  Size of the Power module’s thread stack.

- **CONFIG_APP_POWER_WATCHDOG_TIMEOUT_SECONDS:**
  Defines the watchdog timeout for the module. Must be larger than the message processing timeout.

- **CONFIG_APP_POWER_MSG_PROCESSING_TIMEOUT_SECONDS:**
  Maximum time spent processing a single message.

See the `Kconfig.power` file in the module's directory for more details on the available Kconfig options.

## Kconfig and device tree

- The Power module uses `npm1300_charger` as specified by device tree.
- The two UART devices `uart0_dev` and `uart1_dev` defined in device tree will be enabled or disabled based on VBUS events.

## State machine

The Power module uses a minimal state machine with a single state, **STATE_RUNNING**. In this state, the module:

1. Initializes the charger and optional fuel gauge in the state entry function.
1. Waits for messages to arrive via zbus.
1. Handles battery sample requests and publishes results.
1. Monitors for VBUS events to enable or disable UART devices.
