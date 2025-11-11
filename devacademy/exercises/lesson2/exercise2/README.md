# Lesson 2, Exercise 2: Subscribe to Multiple Channels

## Objective

Create a simple logging module that subscribes to multiple zbus channels and logs all activity. This helps you understand system behavior and is useful for debugging.

## Task Description

Create a new module called `logger` that:
1. Subscribes to all major channels (NETWORK, CLOUD, LOCATION, etc.)
2. Logs when messages are published on any channel
3. Optionally logs message content
4. Helps understand the flow of messages through the system

## Implementation

This exercise is left as an independent challenge. Use the dummy module template as a starting point and apply what you learned in Lesson 2.

## Hints

- Start with the dummy module template
- Use a message subscriber (not listener)
- Subscribe to multiple channels using `ZBUS_CHAN_ADD_OBS()`
- Log the channel name and message type
- Keep logging concise to avoid overwhelming output

## Expected Behavior

```
[logger] <inf> Message on NETWORK_CHAN: type=NETWORK_CONNECTED
[logger] <inf> Message on CLOUD_CHAN: type=CLOUD_CONNECTING
[logger] <inf> Message on LOCATION_CHAN: type=LOCATION_SEARCH_STARTED
[logger] <inf> Message on LOCATION_CHAN: type=LOCATION_SEARCH_DONE
[logger] <inf> Message on CLOUD_CHAN: type=CLOUD_DATA_SENT
```

See `solution/` directory for complete implementation.

