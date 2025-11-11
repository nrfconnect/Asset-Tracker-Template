# Lesson 2, Exercise 1: Add Custom Zbus Event

## Objective

Add VBUS (USB power) detection events to the power module and handle them in the main module by triggering LED patterns. This exercise demonstrates how to add custom zbus events and communicate between modules.

## Prerequisites

- Completed Lesson 2
- Asset Tracker Template built and running
- Thingy:91 X (required for VBUS detection)

## Task Description

You will:
1. Add `POWER_VBUS_CONNECTED` and `POWER_VBUS_DISCONNECTED` message types
2. Publish these events when VBUS status changes
3. Subscribe to power events in the main module
4. Trigger LED patterns in response:
   - **Blue blinking** when USB connected
   - **Purple blinking** when USB disconnected

## Implementation Steps

### Step 1: Define Message Types

Edit `src/modules/power/power.h`:

Add new message types to the enum:
```c
enum power_msg_type {
    /* Existing types */
    POWER_SAMPLE_REQUEST,
    POWER_SAMPLE_RESPONSE,
    
    /* New types - ADD THESE */
    POWER_VBUS_CONNECTED,
    POWER_VBUS_DISCONNECTED,
};
```

### Step 2: Publish VBUS Events

Edit `src/modules/power/power.c`:

Find the `event_callback()` function and modify it to publish events:

```c
static void event_callback(const struct device *dev, uint16_t pins, bool vbus_present)
{
    int err;
    
    if (pins & BIT(NPM13XX_EVENT_VBUS_DETECTED)) {
        LOG_DBG("VBUS detected");
        
        /* TODO: Publish POWER_VBUS_CONNECTED event */
        struct power_msg msg = {
            .type = POWER_VBUS_CONNECTED
        };
        
        err = zbus_chan_pub(&POWER_CHAN, &msg, K_SECONDS(1));
        if (err) {
            LOG_ERR("zbus_chan_pub, error: %d", err);
            SEND_FATAL_ERROR();
            return;
        }
    }
    
    if (pins & BIT(NPM13XX_EVENT_VBUS_REMOVED)) {
        LOG_DBG("VBUS removed");
        
        /* TODO: Publish POWER_VBUS_DISCONNECTED event */
        struct power_msg msg = {
            .type = POWER_VBUS_DISCONNECTED
        };
        
        err = zbus_chan_pub(&POWER_CHAN, &msg, K_SECONDS(1));
        if (err) {
            LOG_ERR("zbus_chan_pub, error: %d", err);
            SEND_FATAL_ERROR();
            return;
        }
    }
    
    /* ... existing code ... */
}
```

### Step 3: Subscribe to Power Channel

Edit `src/modules/main/main.c`:

Add `POWER_CHAN` to the channel list:

```c
#define CHANNEL_LIST(X) \
    X(CLOUD_CHAN, struct cloud_msg) \
    X(BUTTON_CHAN, struct button_msg) \
    X(FOTA_CHAN, enum fota_msg_type) \
    X(NETWORK_CHAN, struct network_msg) \
    X(LOCATION_CHAN, struct location_msg) \
    X(STORAGE_CHAN, struct storage_msg) \
    X(TIMER_CHAN, enum timer_msg_type) \
    X(POWER_CHAN, struct power_msg)  /* ADD THIS */
```

### Step 4: Handle Power Events

Add handling in the `state_running_run()` function:

```c
static enum smf_state_result state_running_run(void *o)
{
    struct main_state *state_object = (struct main_state *)o;
    
    /* ... existing code ... */
    
    /* TODO: Handle power module messages */
    if (state_object->chan == &POWER_CHAN) {
        struct power_msg msg = MSG_TO_POWER_MSG(state_object->msg_buf);
        
        if (msg.type == POWER_VBUS_CONNECTED) {
            LOG_INF("VBUS connected - blue LED");
            
            struct led_msg led_msg = {
                .type = LED_RGB_SET,
                .red = 0,
                .green = 0,
                .blue = 255,
                .duration_on_msec = 300,
                .duration_off_msec = 300,
                .repetitions = 10,
            };
            
            int err = zbus_chan_pub(&LED_CHAN, &led_msg, K_SECONDS(1));
            if (err) {
                LOG_ERR("zbus_chan_pub, error: %d", err);
                SEND_FATAL_ERROR();
            }
            
            return SMF_EVENT_CONSUMED;
        }
        
        if (msg.type == POWER_VBUS_DISCONNECTED) {
            LOG_INF("VBUS disconnected - purple LED");
            
            struct led_msg led_msg = {
                .type = LED_RGB_SET,
                .red = 255,
                .green = 0,
                .blue = 255,
                .duration_on_msec = 1000,
                .duration_off_msec = 700,
                .repetitions = 10,
            };
            
            int err = zbus_chan_pub(&LED_CHAN, &led_msg, K_SECONDS(1));
            if (err) {
                LOG_ERR("zbus_chan_pub, error: %d", err);
                SEND_FATAL_ERROR();
            }
            
            return SMF_EVENT_CONSUMED;
        }
    }
    
    return SMF_EVENT_PROPAGATE;
}
```

### Step 5: Build and Test

Build and flash:
```bash
west build -p -b thingy91x/nrf9151/ns
west flash --erase
```

### Step 6: Verify Behavior

1. Connect USB cable to Thingy:91 X
2. Observe **blue blinking LED** for ~3 seconds
3. Disconnect USB cable
4. Observe **purple blinking LED** for ~7 seconds

Monitor logs:
```
[00:00:10.123] <dbg> power: VBUS detected
[00:00:10.124] <inf> main: VBUS connected - blue LED
[00:00:15.456] <dbg> power: VBUS removed
[00:00:15.457] <inf> main: VBUS disconnected - purple LED
```

## Verification Checklist

- [ ] New message types added to `power.h`
- [ ] VBUS events published in `power.c`
- [ ] POWER_CHAN added to main module channel list
- [ ] Event handling implemented in main module
- [ ] Blue LED blinks when USB connected
- [ ] Purple LED blinks when USB disconnected
- [ ] Logs show event detection and handling

## Expected Results

✅ Blue LED blinks rapidly when USB is connected
✅ Purple LED blinks slowly when USB is disconnected
✅ Logs show VBUS events being detected and handled
✅ System continues normal operation

## Troubleshooting

**LED doesn't blink:**
- Check that you're using Thingy:91 X (VBUS detection requires this hardware)
- Verify LED module is enabled: `CONFIG_APP_LED=y`
- Check logs for error messages

**Events not published:**
- Verify message struct initialization is correct
- Check return value of `zbus_chan_pub()`
- Enable debug logging: `CONFIG_APP_POWER_LOG_LEVEL_DBG=y`

**Main module not receiving events:**
- Verify POWER_CHAN is in the channel list
- Check that main module is subscribing to the channel
- Enable zbus logging: `CONFIG_ZBUS_LOG_LEVEL_DBG=y`

## Bonus Challenges

1. **Add vibration motor control** when VBUS changes
2. **Send VBUS status to cloud** as device telemetry
3. **Change sampling behavior** when on battery vs USB power
4. **Add battery charging detection** and display status

## Key Concepts Reinforced

- Adding new zbus message types
- Publishing messages on events
- Subscribing to channels
- Handling messages in state machine
- Module-to-module communication

## References

- [Zbus Documentation](https://docs.zephyrproject.org/latest/services/zbus/index.html)
- [Lesson 2: Zbus Communication](../../lessons/lesson2_zbus_communication.md)
- [Power Module Documentation](../../../docs/modules/power.md)

## Solution

Complete solution code is available in the `solution/` directory.

Next: Continue to [Exercise 2](../exercise2/README.md) to create a multi-channel logging module.

