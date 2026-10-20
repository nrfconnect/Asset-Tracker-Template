# LED module

The LED module controls the LEDs on the device using pulse width modulation (PWM) and timers to create blinking patterns. For devices with RGB LEDs, the module can set specific colors. For devices without RGB LEDs, the colors map to regular LEDs instead.

## Messages

The LED module uses the zbus channel `led_chan` to receive control commands. Other modules can publish messages to this channel to control the LED behavior.

The module accepts messages with the following parameters:

- **Color values**:

    - `red`: Red component, from 0 to 255.
    - `green`: Green component, from 0 to 255.
    - `blue`: Blue component, from 0 to 255.

- **Timing parameters**:

    - `duration_on_msec`: How long the LED stays on in milliseconds.
    - `duration_off_msec`: How long the LED stays off in milliseconds.
    - `repetitions`: Number of blink cycles, where `-1` means infinite blinking.

The message structure is defined in `led.h`:

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

1. When the module receives a message on the `led_chan` channel, it performs the following steps:

    - Cancels any existing blink pattern.
    - Saves the new LED state, which is the colors and the timing.
    - Turns the LED on with the specified color.

1. When the message specifies a blinking pattern, which means that `repetitions` is not `0`, the module performs the following steps:

    - Starts a timer with `duration_on_msec`.
    - Toggles the LED between the on and off states when the timer expires.
    - Alternates the timer between `duration_on_msec` and `duration_off_msec`.
    - Continues until the LED has blinked the specified number of repetitions.

When `repetitions` is `-1`, the blinking continues indefinitely, or until the module receives a new message.

The module handles error cases gracefully and reports issues through logging.

## Configuration

The LED module uses the following configuration options:

- **CONFIG_APP_LED_LOG_LEVEL**: Controls the logging level for the LED module.
- **Devicetree configuration**: The module needs three PWM LED aliases in the devicetree:

    - `pwm-led0`: The red channel.
    - `pwm-led1`: The green channel.
    - `pwm-led2`: The blue channel.

## Setting LED colors

You can control the LED colors through the LED module using zbus messages. See the following example, which sets a yellow blinking pattern:

```c
/* Set yellow blinking pattern */
struct led_msg led_msg = {
    .type = LED_RGB_SET,
    .red = 255,
    .green = 255,
    .blue = 0,
    .duration_on_msec = 250,
    .duration_off_msec = 2000,
    .repetitions = 10,
};

/* Publish the message to led_chan */
int err = zbus_chan_pub(&led_chan, &led_msg, K_SECONDS(1));
if (err) {
    LOG_ERR("zbus_chan_pub, error: %d", err);
    return;
}
```
