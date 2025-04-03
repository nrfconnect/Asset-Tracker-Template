# Architecture

The Asset Tracker Template application leverages Zephyr features to create a modular, event-driven system. Key to the template architecture are [zbus](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/zbus/index.html) for inter-module communication and the [State Machine Framework](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/smf/index.html) (SMF) for managing module behavior.

This document provides an overview of the architecture and explains how the different modules interact with each other, with a focus on the zbus messaging bus and the State Machine Framework.

# Table of contents

- [Zbus basics](#zbus-basics)
  - [Message subscribers](#message-subscribers)
  - [Listeners](#listeners)
  - [Practical use](#practical-use)
    - [Channels](#channels)
    - [Message types](#message-types)
    - [Observers](#observers)
    - [Message sending](#message-sending)

## Zbus basics

Zbus is a Zephyr messaging bus that enables asynchronous message passing between software components. In the Asset Tracker Template, it allows modules to operate independently while reacting to relevant events form other modules.
Each module can provide own their channels with defined message types, and other modules can subscribe to these channels to receive messages. Other modules may also send messages to these channels to request actions, notify of events, or share data.
Some modules, such as the Button module, does not observe any channels, and acts purely as a message publisher. Other modules, such as the main module, may observe multiple channels and react to messages from different sources.

In the Asset Tracker Template, each module defines the channels and message ypes in their own header file. For example, in `modules/network/network.h`, the Network module defines the `NETWORK_CHAN` channel and the `struct network_msg` message type.

A zbus observer is an entity that listens for messages on a specific channel. There are multiple types of observers, including message subscribers and listeners. These observer types offer message delivery guarantees and are used in different scenarios.
We use only the two zbus observer types in the Asset Tracker Template:

### Message subscribers

Used by modules that have their own thread and that perform actions that may block in response to messages.
For example, the Network module subscribes to its own `NETWORK_CHAN` channel to receive messages about network events. The module may react to a message by sending some AT command to the modem, which may block until some signalling with the network is done and a response is received. This is why the module has its own thread and needs to be a message subscriber.

A message subscriber will queue up messages that are received while the module is busy processing another message. The module can then process the messages in the order they were received. An incoming message can never interrupt the processing of another message. This is a key part of the run-to-completion model that the Asset Tracker Template uses.
See the [zbus documentation](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/zbus/index.html)  for more information on how memory is managed for message subscribers, and for information on how to [configure the memory pool](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/zbus/index.html#configuration_options).

### Listeners

Used by modules that do not have their own thread and that does not block when processing messages. A listener will receive a message synchrounously in the sender's context. For example, the LED module listens for messages on the `LED_CHAN` channel. When it receives an `LED_RGB_SET` message from the main module, it will immediately set the RGB LED color without blocking. This happens in the main module's context. The LED module does not have its own thread and does not block when processing messages, so it can be a listener.

### Practical use

#### Channels

Channels are defined using `ZBUS_CHAN_DEFINE`, specifying the channel name, message type and more. For example:

```c
ZBUS_CHAN_DEFINE(NETWORK_CHAN,                          /* Channel name */
        struct network_msg,                             /* Message type */
        NULL,                                           /* Optional validator function */
        NULL,                                           /* Optional pointer to user data */
        ZBUS_OBSERVERS_EMPTY,                           /* Initial observers */
        ZBUS_MSG_INIT(.type = NETWORK_DISCONNECTED)     /* Message initialization */
);
```

In the above example, initial observers are set to `ZBUS_OBSERVERS_EMPTY` to indicate that no observers are initially listening on the channel. However, opservers can still be added at compile time using `ZBUS_CHAN_ADD_OBS`. It is also possible to add observers at runtime using `zbus_chan_add_obs`. The reason that we need to set the initial observers to `ZBUS_OBSERVERS_EMPTY` is that the Network module is not aware of any other modules in the system, and doing it this way avoids coupling between modules.

As a rule of thumb, modules in the Asset Tracker Template should, when possible, not be aware of each other, and should not have any dependencies between them. This is not always possible, but it is a good practice to strive for. Some modules will have to be aware of each other, such as the Main module, which is the central module that controls the system. The Main module will have to be aware of other modules in the system to implement the business logic of the application.

#### Message types

Message data types may be any valid C type, and their content is specific to the needs of the module. For example from the Network module header file:

```c
struct network_msg {
        enum network_msg_type type;
        union {
                enum lte_lc_system_mode system_mode;
                /* Other message-specific fields */
        };
};
```

There are a couple of things to note in the above example on how message types are defined in the Asset Tracker Template:

- The message type is an enum that is specific to the Network module. The message type is typically used in a switch-case statement in the subscriber to determine what action to take and what, if any, other fields in the message to use.

- When there are multiple message types in a message, it is a good practice to use a union to save memory. This is because the message will be allocated on the stack when it is sent, and it is good to keep the message size as small as possible. We have chosen to use anonymous unions in the Asset Tracker Template to avoid having to write the union name when accessing the union members.

#### Observers

To add a **listener** to a channel, use `ZBUS_LISTENER_DEFINE` and `ZBUS_CHAN_ADD_LISTENER`, such as in this example from the LED module:

```c
ZBUS_LISTENER_DEFINE(led, led_callback);

ZBUS_CHAN_ADD_LISTENER(LED_CHAN, led, 0);
```

The `led_callback` function will be called when a message is sent to the `LED_CHAN` channel. The `led_callback` function should have the following signature:

```c
void led_callback(const struct zbus_msg *msg, void *user_data);
```

**Message subscribers** are defined using `ZBUS_MSG_SUBSCRIBER_DEFINE` and added as observe a specific channel using `ZBUS_CHAN_ADD_OBS`. Subscribers
listen for messages using `zbus_sub_wait_msg`.

```c
ZBUS_MSG_SUBSCRIBER_DEFINE(network_subscriber);

ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, network_subscriber, 0);
```

Messages sent to the `NETWORK_CHAN` channel will be received by the `network_subscriber` and added to the subscriber's message queue. The subscriber can then process the messages in the order they were received using `zbus_sub_wait_msg`.:

```c
struct network_msg msg;

zbus_sub_wait_msg(&network_subscriber, &msg, K_FOREVER);
```

The above code will block until a message is received on the `NETWORK_CHAN` channel. A copy of the message will be stored in the `msg` variable, and the subscriber can then process the message.
In the Asset Tracker Template, the module thread will

#### Message sending

Messages are sent on a channel using `zbus_chan_pub()`. For example, to send a message to the `LED_CHAN` channel:

```c
struct led_msg msg = {
        .type = LED_RGB_SET,
        .red = 255,
        .green = 0,
        .blue = 0,
        .duration_on_msec = 1000,
        .duration_off_msec = 1000,
        .repetisions = 10,
};

zbus_chan_pub(LED_CHAN, &msg);

```

The above code will send a message to the `LED_CHAN` channel with the message type `LED_RGB_SET` and the specified parameters. The LED module will receive the message and call the `led_callback` function with the message data, as described in [Listeners](#listeners).

## State Machine Framework

The State Machine Framework (SMF) is a Zephyr library that provides a way to implement hierarchical state machines. The Asset Tracker Template uses SMF extensively to manage module behavior and state transitions. Several key modules including Network, Cloud, FOTA, and the Main module implement state machines using SMF.

The [documentation on SMF](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/smf/index.html) provides a good introduction, and this section will only cover the parts that are relevant for the Asset Tracker Template.

### Key SMF features

The Asset Tracker Template uses most of the features that SMF offers.

#### State definition

States are defined using the `SMF_CREATE_STATE` macro, which allows specifying:

- **Entry function:** Called when entering the state
- **Run function:** Called while in the state
- **Exit function:** Called when leaving the state
- **Parent state:** For hierarchical state machines
- **Initial transition:** A state may transition to a sub-state upon entry.

Example from the Cloud module:

```c
[STATE_CONNECTED] =
    SMF_CREATE_STATE(state_connected_entry,             /* Entry function */
                     NULL,                              /* Run function */
                     state_connected_exit,              /* Exit function */
                     &states[STATE_RUNNING],            /* Parent state */
                     &states[STATE_CONNECTED_READY]),   /* Initial transition */
```

### State machine context

Each module that uses SMF maintains a context structure, which is usually embedded within a state structure for the module that contains other relevant data for the module's operation.
Example from the cloud module:

```c
struct cloud_state {
        /* This must be first */
        struct smf_ctx ctx;

        /* Last channel type that a message was received on */
        const struct zbus_channel *chan;

        /* Last received message */
        uint8_t msg_buf[MAX_MSG_SIZE];

        /* Network status */
        enum network_msg_type nw_status;

        /* Connection attempt counter. Reset when entering STATE_CONNECTING */
        uint32_t connection_attempts;

        /* Connection backoff time */
        uint32_t backoff_time;
};
```

The SMF context struct member is used to track the current state and manage state transitions. It is passed to all SMF function calls.

### State machine initialization

State machines are initialized to an initial state using `smf_set_initial()`:

```c
smf_set_initial(SMF_CTX(&module_state), &states[STATE_RUNNING]);
```

This has to be done before the state machine is executed the first time.

### State machine execution

The state machine is run using `smf_run_state()`, which:

- Executes the run function of the current state if it is defined
- Executes the run function of parent states unless:
        - A state transition happens
        - A child state marks the message as handled using `smf_state_handled()`
- Executes the exit function of the current and parent states when leaving a state

### State eransitions

Transitions between states are handled using `smf_set_state()`:

```c
smf_set_state(SMF_CTX(state_object), &states[NEW_STATE]);
```

A transition to another state has to be the last thing happening in a state handler. This is to ensure correct order of execution of parent state handlers.
Transitions may happen to any state in the hierarchy, and SMF handles all the exit and entry function executions on the way to the new state.

## Practical use of SMF

### Hierarchical states

The framework supports parent-child state relationships, allowing common behavior to be implemented in parent states. For example, in the Network module:

- `STATE_RUNNING` is the top-level state
- `STATE_DISCONNECTED` and `STATE_CONNECTED` are child states of `STATE_RUNNING`
- `STATE_DISCONNECTED_IDLE` is a child state of `STATE_DISCONNECTED`

This hierarchy allows for shared behavior and clean state organization.

Here is the full state machine of the network module, both graphically and SMF implementation:

![Network module state diagram](../images/network_module_state_machine.png)

```c
static const struct smf_state states[] = {
        [STATE_RUNNING] =
                SMF_CREATE_STATE(state_running_entry, state_running_run, NULL,
                                 NULL,	/* No parent state */
                                 &states[STATE_DISCONNECTED]),
        [STATE_DISCONNECTED] =
                SMF_CREATE_STATE(state_disconnected_entry, state_disconnected_run, NULL,
                                 &states[STATE_RUNNING],
                                 &states[STATE_DISCONNECTED_SEARCHING]),
        [STATE_DISCONNECTED_IDLE] =
                SMF_CREATE_STATE(NULL, state_disconnected_idle_run, NULL,
                                 &states[STATE_DISCONNECTED],
                                 NULL), /* No initial transition */
        [STATE_DISCONNECTED_SEARCHING] =
                SMF_CREATE_STATE(state_disconnected_searching_entry,
                                 state_disconnected_searching_run, NULL,
                                 &states[STATE_DISCONNECTED],
                                 NULL), /* No initial transition */
        [STATE_CONNECTED] =
                SMF_CREATE_STATE(state_connected_entry, state_connected_run, NULL,
                                 &states[STATE_RUNNING],
                                 NULL), /* No initial transition */
        [STATE_DISCONNECTING] =
                SMF_CREATE_STATE(state_disconnecting_entry, state_disconnecting_run, NULL,
                                 &states[STATE_RUNNING],
                                 NULL), /* No initial transition */
};
```

In the image above, the black dots and arrow indicate initial transitions.
In this case, the initial state is set to `STATE_RUNNING`. In the state machine definition, intial transitions are configured, such that the state machine will end up in `STATE_DISCONNECTED_SEARCHING` when entering `STATE_RUNNING`.

### Message handling

Modules combine SMF with zbus to handle events and trigger state transitions. For example, the Network module transitions between states based on network connectivity events:

- Network connected → STATE_CONNECTED
- Network disconnected → STATE_DISCONNECTED
- Network searching → STATE_DISCONNECTED_SEARCHING

### Run-to-completion

The state machine implementation follows a run-to-completion model where:

- Message processing and state machine execution, including transitions, complete fully before processing new messages
- Entry and exit functions are called in the correct order when transitioning states
- Parent state transitions are handled automatically when transitioning between child states

This ensures predictable behavior and proper state cleanup during transitions, as there is no mechanism for interrupting or changing the state machine execution from the outside.
