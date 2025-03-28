# Button module

The button module provides button press event handling functionality for the application. It uses the `dk_buttons_and_leds` library to detect button presses and publishes these events through a zbus channel for other modules to consume.

## Messages

The button module communicates via the zbus channel `BUTTON_CHAN`.

### Output Messages

- The module publishes a message with payload of type `uint8_t` with the button number that has been pressed.
