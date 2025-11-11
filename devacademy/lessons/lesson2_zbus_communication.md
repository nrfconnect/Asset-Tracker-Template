# Lesson 2: Architecture - Zbus Communication

## Lesson Overview

In this lesson, you will learn about the **Zephyr bus (zbus)** messaging system, which is the foundation of inter-module communication in the Asset Tracker Template. You'll understand how modules communicate without being directly coupled, explore message subscribers and listeners, and implement your own custom zbus events.

### Learning Objectives

By the end of this lesson, you will be able to:

- Explain the benefits of message-based communication
- Understand zbus channels and message types
- Differentiate between message subscribers and listeners
- Publish and receive messages on zbus channels
- Add custom events to existing modules
- Subscribe to multiple channels in a module

### Duration

Approximately 90-120 minutes

## 1. Introduction to Event-Driven Architecture

### 1.1 What is Message-Based Communication?

Traditional embedded systems often use **direct function calls** between components:

```c
// Traditional approach - tight coupling
void business_logic(void) {
    if (need_location) {
        location_data_t loc = location_get();  // Direct call
        cloud_send(loc);                        // Direct call
    }
}
```

**Problems with this approach:**
- ❌ **Tight coupling** - Components depend on each other's implementation
- ❌ **Hard to test** - Cannot test components in isolation
- ❌ **Inflexible** - Changing one component affects others
- ❌ **Threading issues** - Difficult to manage when components run in different threads

The Asset Tracker Template uses **message-based communication** instead:

```c
// Message-based approach - loose coupling
void business_logic(void) {
    if (need_location) {
        struct location_msg msg = {
            .type = LOCATION_REQUEST
        };
        zbus_chan_pub(&LOCATION_CHAN, &msg, K_SECONDS(1));
        // Location module receives message and responds when ready
    }
}
```

**Benefits of message-based communication:**
- ✅ **Loose coupling** - Modules don't depend on each other's implementation
- ✅ **Testability** - Can test modules independently
- ✅ **Flexibility** - Easy to add, remove, or replace modules
- ✅ **Thread-safe** - zbus handles synchronization
- ✅ **Observable** - Easy to see what messages flow through the system

### 1.2 Why Zbus?

Zephyr provides several messaging mechanisms:

| Mechanism | Use Case | Complexity |
|-----------|----------|------------|
| **Message Queues** | Point-to-point communication | Medium |
| **Work Queues** | Deferred work execution | Low |
| **Events** | Simple notification flags | Low |
| **Zbus** | Publish-subscribe messaging | Medium |

The Asset Tracker Template uses **zbus** because it provides:

- **Publish-subscribe model** - One publisher, multiple subscribers
- **Type-safe messages** - Compile-time checking of message types
- **Multiple observer types** - Subscribers (queued) and listeners (immediate)
- **Runtime observation** - Can add/remove observers dynamically
- **Channel-based** - Organized by topic/functionality

## 2. Zbus Fundamentals

### 2.1 Core Concepts

Zbus revolves around three main concepts:

```
┌──────────────┐
│  PUBLISHER   │  Sends messages to a channel
└──────┬───────┘
       │
       ▼
┌──────────────┐
│   CHANNEL    │  Named message queue for a specific topic
└──────┬───────┘
       │
       ├─────────────┐
       ▼             ▼
┌──────────┐   ┌─────────┐
│SUBSCRIBER│   │LISTENER │  Receive messages from the channel
└──────────┘   └─────────┘
```

#### Channels

A **channel** is a named message bus for a specific topic. Each channel:
- Has a **unique name** (e.g., `NETWORK_CHAN`, `LOCATION_CHAN`)
- Carries **one message type** (e.g., `struct network_msg`)
- Can have **multiple observers** (subscribers/listeners)
- Maintains **the last published message**

#### Publishers

Any code can **publish** a message to a channel:
- No registration required
- Can publish from any context (thread, ISR)
- Blocked if delivery to subscribers is slow

#### Observers

**Observers** receive messages from channels. There are two types:

**Message Subscribers:**
- Have their **own message queue**
- Messages are **queued** if the subscriber is busy
- Used by modules with **blocking operations**
- Run in their **own thread context**

**Listeners:**
- Receive messages **immediately**
- Called in the **publisher's context**
- Must be **non-blocking**
- Used for **simple, quick operations**

### 2.2 Channel Definition

Channels are defined in module header files using `ZBUS_CHAN_DEFINE`:

```c
// In network.h

/* Define the message structure */
struct network_msg {
    enum network_msg_type type;
    union {
        enum lte_lc_system_mode system_mode;
        /* Other type-specific fields */
    };
};

/* Define the channel */
ZBUS_CHAN_DEFINE(
    NETWORK_CHAN,                               /* Channel name */
    struct network_msg,                         /* Message type */
    NULL,                                       /* Validator function (optional) */
    NULL,                                       /* User data (optional) */
    ZBUS_OBSERVERS_EMPTY,                       /* Initial observers */
    ZBUS_MSG_INIT(.type = NETWORK_DISCONNECTED) /* Initial message */
);
```

**Key points:**
- **Channel name** must be unique across the application
- **Message type** defines what data the channel carries
- **Initial observers** are set to `EMPTY` to avoid coupling
- **Initial message** sets the channel's starting state

### 2.3 Message Types

Messages are typically defined as structures with an **enum type field**:

```c
/* Message type enumeration */
enum network_msg_type {
    /* Output messages (sent by network module) */
    NETWORK_CONNECTED,
    NETWORK_DISCONNECTED,
    NETWORK_CONNECTION_TIMEOUT,
    
    /* Input messages (received by network module) */
    NETWORK_DISCONNECT_REQUEST,
};

/* Message structure */
struct network_msg {
    enum network_msg_type type;
    
    /* Union for type-specific data (saves memory) */
    union {
        enum lte_lc_system_mode system_mode;
        int connection_attempts;
        /* Other fields */
    };
};
```

**Design patterns:**
- Use **enum** for message type discrimination
- Use **anonymous union** for type-specific fields
- Keep messages **small** (allocated on stack)
- Use **output** vs **input** message types to clarify direction

## 3. Message Subscribers

### 3.1 When to Use Subscribers

Use message subscribers for modules that:
- Perform **blocking operations** (modem AT commands, sensor reads, network I/O)
- Need their **own thread** for execution
- Require **queuing** of messages while processing

Examples in ATT:
- **Network module** - Sends AT commands to modem (blocking)
- **Location module** - Acquires GNSS fix (long-running)
- **Cloud module** - Sends data over network (blocking I/O)

### 3.2 Defining a Subscriber

A subscriber is defined in two steps:

**Step 1: Define the subscriber**
```c
// In network.c

/* Define a message subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(network_subscriber);
```

**Step 2: Add subscriber to channel(s)**
```c
/* Subscribe to the NETWORK_CHAN channel */
ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, network_subscriber, 0);

/* Can subscribe to multiple channels */
ZBUS_CHAN_ADD_OBS(BUTTON_CHAN, network_subscriber, 0);
```

### 3.3 Receiving Messages as a Subscriber

Subscribers receive messages in their thread using `zbus_sub_wait_msg()`:

```c
static void network_task(void)
{
    struct network_state state;
    const struct zbus_channel *chan;
    
    while (true) {
        /* Wait for message from any subscribed channel */
        int err = zbus_sub_wait_msg(
            &network_subscriber,        /* Subscriber */
            &chan,                      /* Output: which channel */
            state.msg_buf,              /* Output: message data */
            K_FOREVER                   /* Timeout */
        );
        
        if (err == -ENOMSG) {
            continue;  /* Timeout, no message */
        }
        
        /* Identify which channel the message came from */
        if (chan == &NETWORK_CHAN) {
            struct network_msg *msg = (struct network_msg *)state.msg_buf;
            
            /* Handle message based on type */
            switch (msg->type) {
                case NETWORK_DISCONNECT_REQUEST:
                    /* Handle disconnect */
                    break;
                default:
                    break;
            }
        }
    }
}
```

**Key points:**
- `zbus_sub_wait_msg()` **blocks** until a message arrives or timeout
- Messages are **queued** if multiple arrive while processing
- The function returns **which channel** the message came from
- Message data is **copied** into the provided buffer

### 3.4 Subscriber Message Queue

Each subscriber has a **message queue** configured via Kconfig:

```kconfig
# Default queue size
CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_POOL_SIZE=8
```

If the queue fills up:
- New messages **block the publisher**
- **Deadlock risk** if not sized properly

Best practices:
- Size queue based on expected message rate
- Process messages quickly
- Use work queues for long operations

## 4. Listeners

### 4.1 When to Use Listeners

Use listeners for modules that:
- Perform **non-blocking operations** (setting LED, logging)
- Execute **very quickly** (< 1 ms)
- Don't need their **own thread**

Examples in ATT:
- **LED module** - Just sets RGB LED state
- **Button module** - Publishes button events (runs in ISR context handled by GPIO driver)

**Warning:** Listeners run in the **publisher's context**, so:
- ❌ Don't block or sleep
- ❌ Don't perform long operations
- ❌ Don't call blocking APIs

### 4.2 Defining a Listener

A listener is defined with a **callback function**:

**Step 1: Define the callback**
```c
// In led.c

static void led_callback(const struct zbus_channel *chan)
{
    /* Get the message from the channel */
    const struct led_msg *msg;
    
    if (zbus_chan_read(chan, &msg, K_NO_WAIT) != 0) {
        return;
    }
    
    /* Handle message */
    switch (msg->type) {
        case LED_RGB_SET:
            set_led_color(msg->red, msg->green, msg->blue);
            break;
        default:
            break;
    }
}
```

**Step 2: Define and register the listener**
```c
/* Define listener with callback */
ZBUS_LISTENER_DEFINE(led_listener, led_callback);

/* Add listener to channel */
ZBUS_CHAN_ADD_OBS(LED_CHAN, led_listener, 0);
```

### 4.3 Listener Execution

When a message is published:

```
Publisher Thread
│
├─> zbus_chan_pub(&LED_CHAN, &msg, K_SECONDS(1))
│   │
│   ├─> led_callback() called IMMEDIATELY
│   │   └─> set_led_color() executes
│   │
│   └─> zbus_chan_pub() returns
│
└─> Continue execution
```

**Key points:**
- Listener executes **synchronously** in publisher's context
- Publisher is **blocked** until listener completes
- Multiple listeners execute **sequentially**
- Listener **cannot block** or sleep

## 5. Publishing Messages

### 5.1 Basic Publishing

Any code can publish a message using `zbus_chan_pub()`:

```c
/* Create message */
struct location_msg msg = {
    .type = LOCATION_REQUEST,
    .timeout_seconds = 60,
};

/* Publish to channel */
int err = zbus_chan_pub(
    &LOCATION_CHAN,     /* Channel to publish to */
    &msg,               /* Message data (copied) */
    K_SECONDS(1)        /* Timeout if observers are busy */
);

if (err) {
    LOG_ERR("Failed to publish message: %d", err);
}
```

### 5.2 Publishing Patterns

#### Pattern 1: Request-Response

One module requests something, another responds:

```c
// Main module requests location
struct location_msg req = {
    .type = LOCATION_REQUEST,
};
zbus_chan_pub(&LOCATION_CHAN, &req, K_SECONDS(1));

// Later, location module responds
struct location_msg resp = {
    .type = LOCATION_RESPONSE,
    .latitude = 63.4305,
    .longitude = 10.3951,
};
zbus_chan_pub(&LOCATION_CHAN, &resp, K_SECONDS(1));
```

#### Pattern 2: Status Notification

A module broadcasts its state changes:

```c
// Network module notifies when connected
struct network_msg msg = {
    .type = NETWORK_CONNECTED,
    .system_mode = LTE_LC_SYSTEM_MODE_LTEM,
};
zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));

// Multiple modules can observe this change
// - Main module adjusts business logic
// - Cloud module initiates connection
// - LED module changes indication
```

#### Pattern 3: Command

One module commands another to perform an action:

```c
// Main module commands LED
struct led_msg msg = {
    .type = LED_RGB_SET,
    .red = 255,
    .green = 0,
    .blue = 0,
    .duration_on_msec = 1000,
    .duration_off_msec = 1000,
    .repetitions = 10,
};
zbus_chan_pub(&LED_CHAN, &msg, K_SECONDS(1));
```

### 5.3 Publishing from ISRs

You can publish from interrupt context, but use **no-wait timeout**:

```c
void button_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    struct button_msg msg = {
        .type = BUTTON_PRESSED,
    };
    
    /* Use K_NO_WAIT in ISR context */
    zbus_chan_pub(&BUTTON_CHAN, &msg, K_NO_WAIT);
}
```

**Important:** If a listener blocks, publishing from ISR will fail with `-EAGAIN`.

## 6. Practical Example: Adding a Custom Event

Let's walk through a practical example of adding a custom event to the power module.

### 6.1 Scenario

We want to:
- Detect when VBUS (USB power) is connected or disconnected on Thingy:91 X
- Notify other modules about these events
- Have the main module trigger LED patterns in response

### 6.2 Step 1: Define Message Types

Edit `src/modules/power/power.h`:

```c
enum power_msg_type {
    /* Existing types */
    POWER_SAMPLE_REQUEST,
    POWER_SAMPLE_RESPONSE,
    
    /* New types */
    POWER_VBUS_CONNECTED,
    POWER_VBUS_DISCONNECTED,
};
```

### 6.3 Step 2: Publish Events

Edit `src/modules/power/power.c`, modify the `event_callback()` function:

```c
static void event_callback(const struct device *dev, uint16_t pins, bool vbus_present)
{
    int err;
    
    if (pins & BIT(NPM13XX_EVENT_VBUS_DETECTED)) {
        LOG_DBG("VBUS detected");
        
        /* Publish VBUS connected event */
        struct power_msg msg = {
            .type = POWER_VBUS_CONNECTED
        };
        
        err = zbus_chan_pub(&POWER_CHAN, &msg, K_SECONDS(1));
        if (err) {
            LOG_ERR("Failed to publish VBUS connected: %d", err);
            SEND_FATAL_ERROR();
        }
    }
    
    if (pins & BIT(NPM13XX_EVENT_VBUS_REMOVED)) {
        LOG_DBG("VBUS removed");
        
        /* Publish VBUS disconnected event */
        struct power_msg msg = {
            .type = POWER_VBUS_DISCONNECTED
        };
        
        err = zbus_chan_pub(&POWER_CHAN, &msg, K_SECONDS(1));
        if (err) {
            LOG_ERR("Failed to publish VBUS disconnected: %d", err);
            SEND_FATAL_ERROR();
        }
    }
    
    /* ... existing code ... */
}
```

### 6.4 Step 3: Subscribe in Another Module

Edit `src/modules/main/main.c`:

**Add channel to subscription list:**
```c
#define CHANNEL_LIST(X)         \
    X(CLOUD_CHAN,  struct cloud_msg)    \
    X(BUTTON_CHAN, struct button_msg)   \
    X(NETWORK_CHAN, struct network_msg) \
    X(LOCATION_CHAN, struct location_msg) \
    X(POWER_CHAN, struct power_msg)     /* Add this */
```

**Handle messages in state machine:**
```c
static enum smf_state_result state_running_run(void *o)
{
    struct main_state *state = (struct main_state *)o;
    
    /* ... existing code ... */
    
    /* Handle power module messages */
    if (state->chan == &POWER_CHAN) {
        struct power_msg msg = MSG_TO_POWER_MSG(state->msg_buf);
        
        if (msg.type == POWER_VBUS_CONNECTED) {
            LOG_INF("VBUS connected - show blue LED");
            
            struct led_msg led = {
                .type = LED_RGB_SET,
                .red = 0,
                .green = 0,
                .blue = 255,
                .duration_on_msec = 300,
                .duration_off_msec = 300,
                .repetitions = 10,
            };
            
            zbus_chan_pub(&LED_CHAN, &led, K_SECONDS(1));
            return SMF_EVENT_CONSUMED;
        }
        
        if (msg.type == POWER_VBUS_DISCONNECTED) {
            LOG_INF("VBUS disconnected - show purple LED");
            
            struct led_msg led = {
                .type = LED_RGB_SET,
                .red = 255,
                .green = 0,
                .blue = 255,
                .duration_on_msec = 1000,
                .duration_off_msec = 700,
                .repetitions = 10,
            };
            
            zbus_chan_pub(&LED_CHAN, &led, K_SECONDS(1));
            return SMF_EVENT_CONSUMED;
        }
    }
    
    return SMF_EVENT_PROPAGATE;
}
```

### 6.5 Step 4: Test

1. Build and flash the modified application
2. Connect/disconnect USB cable to Thingy:91 X
3. Observe LED patterns change:
   - **Blue blinking** when USB connected
   - **Purple blinking** when USB disconnected

## 7. Best Practices

### 7.1 Channel Design

✅ **DO:**
- Create separate channels for different concerns
- Use descriptive channel names (e.g., `NETWORK_CHAN`, not `CHAN1`)
- Define channels in the owning module's header file
- Keep initial observers empty for loose coupling

❌ **DON'T:**
- Reuse channels for unrelated purposes
- Create too many channels (adds overhead)
- Couple modules by hardcoding observers

### 7.2 Message Design

✅ **DO:**
- Use enum for message type discrimination
- Use anonymous unions for type-specific fields
- Keep messages small (they're copied)
- Document input vs output message types

❌ **DON'T:**
- Use pointers in messages (ownership unclear)
- Make messages unnecessarily large
- Use different structs for the same channel

### 7.3 Publishing

✅ **DO:**
- Check return value of `zbus_chan_pub()`
- Use appropriate timeout values
- Use `K_NO_WAIT` in ISR context
- Log errors on publish failure

❌ **DON'T:**
- Ignore publish errors
- Use long timeouts in time-critical code
- Publish from ISR with `K_FOREVER` timeout

### 7.4 Subscribers vs Listeners

| Scenario | Use Subscriber | Use Listener |
|----------|---------------|--------------|
| Blocking operations | ✅ Yes | ❌ No |
| Quick, non-blocking | ❌ No | ✅ Yes |
| Need message queuing | ✅ Yes | ❌ No |
| Run in own thread | ✅ Yes | ❌ No |
| Simple state changes | ❌ No | ✅ Yes |

## 8. Debugging Zbus Communication

### 8.1 Enabling Zbus Logging

Enable detailed zbus logging in `prj.conf`:

```kconfig
CONFIG_ZBUS_LOG_LEVEL_DBG=y
```

This shows:
- When messages are published
- Which observers receive them
- Delivery times

### 8.2 Runtime Observation

You can observe channels at runtime for debugging:

```c
/* Define an observer that logs all messages */
static void debug_listener(const struct zbus_channel *chan)
{
    LOG_INF("Message on channel: %s", zbus_chan_name(chan));
}

ZBUS_LISTENER_DEFINE(debug, debug_listener);

/* Add to any channel you want to monitor */
ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, debug, 0);
```

### 8.3 Common Issues

**Problem:** Publisher blocks indefinitely
- **Cause:** Subscriber queue full or listener blocking
- **Solution:** Increase queue size or use shorter timeout

**Problem:** Messages not received
- **Cause:** Observer not registered on channel
- **Solution:** Verify `ZBUS_CHAN_ADD_OBS()` is called

**Problem:** Wrong message data
- **Cause:** Message buffer too small
- **Solution:** Ensure buffer is `>= sizeof(message_type)`

## Summary

In this lesson, you learned:

✅ Message-based communication provides loose coupling between modules
✅ Zbus uses a publish-subscribe model with channels and observers
✅ Message subscribers queue messages for blocking operations
✅ Listeners receive messages immediately for quick operations
✅ Messages are published with `zbus_chan_pub()`
✅ Custom events can be added by defining messages and publishing them
✅ Proper channel and message design is critical for maintainability

## Exercises

### Exercise 1: Add Custom Zbus Event

Implement the VBUS detection feature described in Section 6.

See [Exercise 1 instructions](../exercises/lesson2/exercise1/README.md)

### Exercise 2: Subscribe to Multiple Channels

Create a simple logging module that subscribes to multiple channels and logs all activity.

See [Exercise 2 instructions](../exercises/lesson2/exercise2/README.md)

## Next Steps

In the next lesson, you'll learn about the **State Machine Framework (SMF)**, which most modules use to manage their behavior and handle zbus messages in a structured way.

Continue to [Lesson 3: Architecture - State Machine Framework](lesson3_state_machine_framework.md)

