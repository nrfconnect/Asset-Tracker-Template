# Button module

The button module provides button press event handling functionality for the application. It uses the `dk_buttons_and_leds` library to detect button presses and publishes these events through a zbus channel for other modules to consume.

## Messages

### Output Messages

- **BUTTON_CHAN**: The module publishes button events through the `BUTTON_CHAN`.
