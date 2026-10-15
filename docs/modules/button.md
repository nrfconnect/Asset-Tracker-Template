# Button module

The Button module provides button press event handling functionality for the application. It uses the `dk_buttons_and_leds` library to detect button presses and publishes these events through a zbus channel for other modules to consume.

## Messages

The Button module communicates through the zbus channel `button_chan`.

### Output messages

The module publishes messages of type `button_msg` containing the following fields:

- `type`: The type of button press, as described in [Message types](#message-types).
- `button_number`: The button number that was pressed.

### Message types

The `type` field takes one of the following values:

- **BUTTON_PRESS_SHORT**: A short button press. The module sends this message when a button is released before the long press timeout expires.
- **BUTTON_PRESS_LONG**: A long button press. The module sends this message when a button is held for the duration of the long press timeout, which you configure with `CONFIG_APP_BUTTON_LONG_PRESS_TIMEOUT_MS`.
