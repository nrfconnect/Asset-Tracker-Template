# LED module

The LED module controls the LEDs on the device using PWM (Pulse Width Modulation) and timers to created blinking patterns. For devices with RGB LEDs, the module can set specific colors. For devices without RGB LEDs, the colors will map to regular LEDs instead.

## Messages

The LED module uses the zbus channel `LED_CHAN` to receive control commands. Other modules can publish messages to this channel to control the LED behavior.

The module accepts messages with the following parameters:

- **Color Values**

    - `red`: Red component (0-255)
    - `green`: Green component (0-255)
    - `blue`: Blue component (0-255)

- **Timing Parameters**

    - `duration_on_msec`: How long the LED stays on in milliseconds.
    - `duration_off_msec`: How long the LED stays off in milliseconds.
    - `repetitions`: Number of blink cycles (-1 for infinite blinking).

The message structure is defined in `led_module.h`:

```c
struct led_msg {
    enum led_msg_type type;

	/** RGB values (0 to 255) */
	uint8_t red;
	uint8_t green;
	uint8_t blue;

	/** Duration of the RGB on/off cycle */
	uint32_t duration_on_msec;
	uint32_t duration_off_msec;

	/** Number of on/off cycles (-1 indicates forever) */
	int repetitions;
};
```

## Operation

Instead of using a state machine, the LED module operates as follows:

1. When a message is received on the `LED_CHAN` channel:

    - Any existing blink pattern is canceled.
    - The new LED state (colors and timing) is saved.
    - The LED is turned on with the specified color.

1. If a blinking pattern is specified (repetitions != 0):

    - A timer is started with `duration_on_msec`.
    - When the timer expires, the LED toggles between on and off states.
    - The timer alternates between `duration_on_msec` and `duration_off_msec`.
    - This continues until the specified number of repetitions is reached.
    - If repetitions is -1, the blinking continues indefinitely or until a new message is received.

The module handles error cases gracefully and reports issues through logging.

## Configuration

The LED module uses the following configuration options:

- **CONFIG_APP_LED_LOG_LEVEL:**
  Controls logging level for the LED module.

- **Devicetree Configuration:**
  The module requires three PWM LED aliases in the devicetree:

    - `pwm-led0`: Red channel
    - `pwm-led1`: Green channel
    - `pwm-led2`: Blue channel
