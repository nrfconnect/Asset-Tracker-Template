# Button module

The Button module provides button press event handling functionality for the application. It uses the `dk_buttons_and_leds` library to detect button presses and publishes these events through a zbus channel for other modules to consume.

## Messages

The Button module communicates through the zbus channel `BUTTON_CHAN`.

### Output messages

The module publishes a message with a payload of type `uint8_t` with the button number that has been pressed.
