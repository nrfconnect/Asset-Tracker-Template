# Lesson 3: Architecture - State Machine Framework

## Lesson Overview

In this lesson, you will learn about the **State Machine Framework (SMF)**, which modules use to manage their internal behavior and state transitions. You'll understand the run-to-completion model, hierarchical state machines, and how SMF integrates with zbus messaging to create predictable, maintainable module behavior.

### Learning Objectives

By the end of this lesson, you will be able to:

- Explain the benefits of state machine-based design
- Understand SMF's run-to-completion model
- Define states with entry, run, and exit functions
- Implement state transitions
- Work with hierarchical state machines
- Integrate SMF with zbus message handling
- Analyze and modify existing state machines

### Duration

Approximately 90-120 minutes

## 1. Introduction to State Machines

### 1.1 Why State Machines?

Embedded systems often involve complex sequences of operations with many possible states. Without structure, this leads to:

❌ **Spaghetti code:**
```c
// Unstructured approach - hard to understand
static bool connected = false;
static bool connecting = false;
static int retry_count = 0;

void network_task(void) {
    while (true) {
        if (!connected && !connecting) {
            if (retry_count < MAX_RETRIES) {
                connecting = true;
                modem_connect();
            }
        }
        
        if (connecting) {
            if (modem_is_connected()) {
                connected = true;
                connecting = false;
                retry_count = 0;
            } else if (modem_has_error()) {
                connecting = false;
                retry_count++;
            }
        }
        
        if (connected && !modem_is_connected()) {
            connected = false;
            // What about connecting flag?
        }
        // ... more complex logic ...
    }
}
```

**Problems:**
- State scattered across multiple variables
- Unclear transitions between states
- Easy to create invalid state combinations
- Difficult to add new states
- Hard to test and debug

✅ **State machine approach:**
```c
// Structured state machine - clear and maintainable
enum states {
    STATE_DISCONNECTED,
    STATE_CONNECTING,
    STATE_CONNECTED,
};

// Current state is explicit
static enum states current_state = STATE_DISCONNECTED;

// State transition is explicit
void transition_to(enum states new_state) {
    current_state = new_state;
}

// Each state has clear behavior
void state_machine_run(void) {
    switch (current_state) {
        case STATE_DISCONNECTED:
            // Handle disconnected state
            break;
        case STATE_CONNECTING:
            // Handle connecting state
            break;
        case STATE_CONNECTED:
            // Handle connected state
            break;
    }
}
```

### 1.2 State Machine Benefits

✅ **Clarity** - Current system state is explicit
✅ **Predictability** - Well-defined transitions
✅ **Testability** - Easy to test each state independently
✅ **Maintainability** - Easy to add/modify states
✅ **Documentation** - State diagrams serve as documentation
✅ **Debugging** - Can log state transitions

### 1.3 State Machine Framework (SMF)

Zephyr's State Machine Framework provides:

- **Hierarchical state machines** - Parent-child relationships
- **Entry/Exit functions** - Automatic cleanup on transitions
- **Run-to-completion** - No interruption during state execution
- **State history** - Can return to previous states
- **Type safety** - Compile-time checking

The Asset Tracker Template uses SMF in most modules:
- Network module (connection management)
- Cloud module (cloud connection lifecycle)
- Location module (positioning operations)
- FOTA module (firmware update process)
- Main module (business logic coordination)

## 2. SMF Fundamentals

### 2.1 State Definition

States are defined using the `SMF_CREATE_STATE` macro:

```c
static const struct smf_state states[] = {
    [STATE_EXAMPLE] = SMF_CREATE_STATE(
        state_example_entry,      /* Entry function (optional) */
        state_example_run,        /* Run function (optional) */
        state_example_exit,       /* Exit function (optional) */
        &states[PARENT_STATE],    /* Parent state (optional) */
        &states[INITIAL_STATE]    /* Initial transition (optional) */
    ),
};
```

**Each component:**

- **Entry function** - Called once when entering the state
- **Run function** - Called for each event/message in this state
- **Exit function** - Called once when leaving the state
- **Parent state** - For hierarchical organization (can be NULL)
- **Initial transition** - Auto-transition to a sub-state (can be NULL)

### 2.2 State Function Signatures

State functions have a specific signature:

```c
/* Entry function */
static void state_example_entry(void *obj)
{
    struct module_state *state = (struct module_state *)obj;
    
    /* Initialization when entering this state */
    LOG_INF("Entering EXAMPLE state");
    state->retry_count = 0;
}

/* Run function */
static enum smf_state_result state_example_run(void *obj)
{
    struct module_state *state = (struct module_state *)obj;
    
    /* Handle events/messages in this state */
    
    return SMF_EVENT_CONSUMED;     /* or SMF_EVENT_PROPAGATE */
}

/* Exit function */
static void state_example_exit(void *obj)
{
    struct module_state *state = (struct module_state *)obj;
    
    /* Cleanup when leaving this state */
    LOG_INF("Exiting EXAMPLE state");
    cleanup_resources();
}
```

### 2.3 State Machine Context

Every state machine has a **context structure** containing:

```c
struct module_state {
    /* MUST be first - SMF context */
    struct smf_ctx ctx;
    
    /* Module-specific data */
    int retry_count;
    uint32_t backoff_time;
    bool is_configured;
    
    /* Zbus integration */
    const struct zbus_channel *chan;
    uint8_t msg_buf[MAX_MSG_SIZE];
};
```

**Key points:**
- `struct smf_ctx` **must be the first member**
- Contains module-specific state data
- Often includes zbus channel and message buffer
- Passed to all state functions as `void *obj`

### 2.4 State Machine Initialization

Initialize the state machine before first use:

```c
static void module_task(void)
{
    struct module_state state = {0};
    
    /* Initialize to the initial state */
    smf_set_initial(SMF_CTX(&state), &states[STATE_INIT]);
    
    /* State machine is now ready to run */
    /* ... event loop ... */
}
```

## 3. Run-to-Completion Model

### 3.1 What is Run-to-Completion?

The **run-to-completion** model means:

> Once state machine execution starts (via `smf_run_state()`), it completes fully before processing the next event.

```
Message arrives
│
├─> smf_run_state() called
│   │
│   ├─> Run current state's run function
│   ├─> Check if state transition requested
│   ├─> If transition:
│   │   ├─> Call current state's exit function
│   │   ├─> Call parent states' exit functions (if any)
│   │   ├─> Call new state's entry function
│   │   └─> Call parent states' entry functions (if any)
│   │
│   └─> smf_run_state() returns
│
└─> Ready for next message
```

**Benefits:**
✅ **Predictable** - No interruption during state execution
✅ **Safe** - State transitions complete atomically
✅ **Simple** - No need for locking or complex synchronization

**Implications:**
- State functions should be **fast**
- Long operations should be **split** across multiple calls
- Cannot **interrupt** state machine execution externally

### 3.2 Event Processing

State machine processes events in its run function:

```c
static enum smf_state_result state_running_run(void *obj)
{
    struct module_state *state = (struct module_state *)obj;
    
    /* Identify the message source */
    if (state->chan == &NETWORK_CHAN) {
        struct network_msg *msg = (struct network_msg *)state->msg_buf;
        
        /* Process message */
        if (msg->type == NETWORK_CONNECTED) {
            /* Transition to connected state */
            smf_set_state(SMF_CTX(state), &states[STATE_CONNECTED]);
            return SMF_EVENT_CONSUMED;  /* Don't propagate to parent */
        }
    }
    
    /* Let parent state handle it */
    return SMF_EVENT_PROPAGATE;
}
```

**Return values:**
- `SMF_EVENT_CONSUMED` - Event handled, don't propagate to parent
- `SMF_EVENT_PROPAGATE` - Let parent state handle event

### 3.3 State Transitions

Transitions are performed with `smf_set_state()`:

```c
/* Transition to a new state */
smf_set_state(SMF_CTX(state), &states[STATE_CONNECTED]);
```

**Important:** State transition should be the **last operation** in a run function:

```c
/* CORRECT - transition last */
static enum smf_state_result state_connecting_run(void *obj)
{
    if (connection_successful) {
        prepare_for_connected_state();  /* Do work first */
        smf_set_state(SMF_CTX(obj), &states[STATE_CONNECTED]);  /* Then transition */
        return SMF_EVENT_CONSUMED;
    }
    return SMF_EVENT_PROPAGATE;
}

/* WRONG - code after transition won't execute properly */
static enum smf_state_result state_connecting_run(void *obj)
{
    if (connection_successful) {
        smf_set_state(SMF_CTX(obj), &states[STATE_CONNECTED]);
        prepare_for_connected_state();  /* BAD - after transition! */
        return SMF_EVENT_CONSUMED;
    }
    return SMF_EVENT_PROPAGATE;
}
```

## 4. Hierarchical State Machines

### 4.1 Why Hierarchical States?

Hierarchical states allow **sharing common behavior** among related states:

```
STATE_RUNNING (parent)
├── Handle common operations for all sub-states
├── STATE_DISCONNECTED (child)
│   ├── Handle disconnected-specific logic
│   └── STATE_DISCONNECTED_IDLE (grandchild)
└── STATE_CONNECTED (child)
    └── Handle connected-specific logic
```

**Benefits:**
- **Code reuse** - Common logic in parent states
- **Organization** - Group related states
- **Flexibility** - Easy to add new sub-states

### 4.2 Parent-Child Relationships

Define hierarchy by setting parent in state definition:

```c
static const struct smf_state states[] = {
    /* Top-level state - no parent */
    [STATE_RUNNING] = SMF_CREATE_STATE(
        state_running_entry,
        state_running_run,
        NULL,
        NULL,                           /* No parent */
        &states[STATE_DISCONNECTED]     /* Initial sub-state */
    ),
    
    /* Child state of STATE_RUNNING */
    [STATE_DISCONNECTED] = SMF_CREATE_STATE(
        state_disconnected_entry,
        state_disconnected_run,
        NULL,
        &states[STATE_RUNNING],         /* Parent */
        &states[STATE_DISCONNECTED_IDLE]
    ),
    
    /* Grandchild state */
    [STATE_DISCONNECTED_IDLE] = SMF_CREATE_STATE(
        NULL,
        state_disconnected_idle_run,
        NULL,
        &states[STATE_DISCONNECTED],    /* Parent */
        NULL
    ),
    
    /* Another child state of STATE_RUNNING */
    [STATE_CONNECTED] = SMF_CREATE_STATE(
        state_connected_entry,
        state_connected_run,
        NULL,
        &states[STATE_RUNNING],         /* Same parent */
        NULL
    ),
};
```

### 4.3 Event Propagation

When processing events, SMF uses this order:

1. **Current state's run function** executes
2. If returns `SMF_EVENT_PROPAGATE`:
   - **Parent state's run function** executes
   - If parent also returns `SMF_EVENT_PROPAGATE`:
     - **Grandparent state's run function** executes
     - And so on up the hierarchy

```
Message arrives
│
├─> Run STATE_DISCONNECTED_IDLE run function
│   ├─> Returns SMF_EVENT_PROPAGATE
│   │
│   └─> Run STATE_DISCONNECTED run function
│       ├─> Returns SMF_EVENT_PROPAGATE
│       │
│       └─> Run STATE_RUNNING run function
│           └─> Handles message or propagates
│
└─> Event processing complete
```

**This allows:**
- Child states to handle specific cases
- Parent states to handle common cases
- Flexibility in event handling

### 4.4 Entry/Exit with Hierarchy

When transitioning between states, **all necessary entry/exit functions are called**:

Example: Transitioning from `STATE_DISCONNECTED_IDLE` to `STATE_CONNECTED`:

```
Current: STATE_DISCONNECTED_IDLE
Target:  STATE_CONNECTED

Execution order:
1. Exit STATE_DISCONNECTED_IDLE
2. Exit STATE_DISCONNECTED
3. Enter STATE_CONNECTED

(STATE_RUNNING is common parent, so not exited/entered)
```

Example: Transitioning from `STATE_CONNECTED` to `STATE_DISCONNECTED_IDLE`:

```
Current: STATE_CONNECTED
Target:  STATE_DISCONNECTED_IDLE

Execution order:
1. Exit STATE_CONNECTED
2. Enter STATE_DISCONNECTED
3. Enter STATE_DISCONNECTED_IDLE

(STATE_RUNNING is common parent, so not exited/entered)
```

## 5. Network Module State Machine Example

### 5.1 Network Module State Diagram

The network module has the following state machine:

```
                       ┌──────────────┐
                       │STATE_RUNNING │ (parent)
                       └───┬──────────┘
                           │
         ┌─────────────────┼─────────────────┐
         │                 │                 │
    ┌────▼───────┐   ┌────▼──────┐    ┌────▼─────────┐
    │DISCONNECTED│   │ CONNECTED │    │DISCONNECTING │
    │  (parent)  │   │           │    │              │
    └─┬──────────┘   └───────────┘    └──────────────┘
      │
      ├───► DISCONNECTED_IDLE
      │
      └───► DISCONNECTED_SEARCHING
```

**State descriptions:**

| State | Purpose |
|-------|---------|
| `STATE_RUNNING` | Top-level state, handles common operations |
| `STATE_DISCONNECTED` | LTE is disconnected (parent of sub-states) |
| `STATE_DISCONNECTED_IDLE` | Not searching for network |
| `STATE_DISCONNECTED_SEARCHING` | Actively searching for network |
| `STATE_CONNECTED` | LTE is connected |
| `STATE_DISCONNECTING` | Disconnecting from LTE |

### 5.2 State Definitions

```c
static const struct smf_state states[] = {
    [STATE_RUNNING] = SMF_CREATE_STATE(
        state_running_entry,
        state_running_run,
        NULL,
        NULL,                               /* No parent */
        &states[STATE_DISCONNECTED]         /* Start disconnected */
    ),
    
    [STATE_DISCONNECTED] = SMF_CREATE_STATE(
        state_disconnected_entry,
        state_disconnected_run,
        NULL,
        &states[STATE_RUNNING],             /* Parent */
        &states[STATE_DISCONNECTED_SEARCHING]  /* Start searching */
    ),
    
    [STATE_DISCONNECTED_IDLE] = SMF_CREATE_STATE(
        NULL,
        state_disconnected_idle_run,
        NULL,
        &states[STATE_DISCONNECTED],        /* Parent */
        NULL
    ),
    
    [STATE_DISCONNECTED_SEARCHING] = SMF_CREATE_STATE(
        state_disconnected_searching_entry,
        state_disconnected_searching_run,
        NULL,
        &states[STATE_DISCONNECTED],        /* Parent */
        NULL
    ),
    
    [STATE_CONNECTED] = SMF_CREATE_STATE(
        state_connected_entry,
        state_connected_run,
        NULL,
        &states[STATE_RUNNING],             /* Parent */
        NULL
    ),
    
    [STATE_DISCONNECTING] = SMF_CREATE_STATE(
        state_disconnecting_entry,
        state_disconnecting_run,
        NULL,
        &states[STATE_RUNNING],             /* Parent */
        NULL
    ),
};
```

### 5.3 State Implementations

**STATE_RUNNING - Common operations**

```c
static enum smf_state_result state_running_run(void *obj)
{
    struct network_state *state = (struct network_state *)obj;
    
    /* Handle messages common to all sub-states */
    if (state->chan == &NETWORK_CHAN) {
        struct network_msg *msg = (struct network_msg *)state->msg_buf;
        
        /* Disconnect request can happen in any state */
        if (msg->type == NETWORK_DISCONNECT_REQUEST) {
            smf_set_state(SMF_CTX(state), &states[STATE_DISCONNECTING]);
            return SMF_EVENT_CONSUMED;
        }
    }
    
    /* Let child states handle other messages */
    return SMF_EVENT_PROPAGATE;
}
```

**STATE_DISCONNECTED_SEARCHING - Connecting to network**

```c
static void state_disconnected_searching_entry(void *obj)
{
    LOG_INF("Searching for LTE network...");
    
    /* Start LTE connection */
    lte_lc_connect_async(lte_handler);
}

static enum smf_state_result state_disconnected_searching_run(void *obj)
{
    struct network_state *state = (struct network_state *)obj;
    
    if (state->chan == &NETWORK_CHAN) {
        struct network_msg *msg = (struct network_msg *)state->msg_buf;
        
        if (msg->type == NETWORK_CONNECTED) {
            /* Connection successful */
            smf_set_state(SMF_CTX(state), &states[STATE_CONNECTED]);
            return SMF_EVENT_CONSUMED;
        }
        
        if (msg->type == NETWORK_CONNECTION_TIMEOUT) {
            /* Connection failed, go idle */
            smf_set_state(SMF_CTX(state), &states[STATE_DISCONNECTED_IDLE]);
            return SMF_EVENT_CONSUMED;
        }
    }
    
    return SMF_EVENT_PROPAGATE;
}
```

**STATE_CONNECTED - Connected to network**

```c
static void state_connected_entry(void *obj)
{
    LOG_INF("Connected to LTE network");
    
    /* Notify other modules */
    struct network_msg msg = {
        .type = NETWORK_CONNECTED
    };
    zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));
}

static enum smf_state_result state_connected_run(void *obj)
{
    struct network_state *state = (struct network_state *)obj;
    
    if (state->chan == &NETWORK_CHAN) {
        struct network_msg *msg = (struct network_msg *)state->msg_buf;
        
        if (msg->type == NETWORK_DISCONNECTED) {
            /* Lost connection */
            smf_set_state(SMF_CTX(state), &states[STATE_DISCONNECTED]);
            return SMF_EVENT_CONSUMED;
        }
    }
    
    return SMF_EVENT_PROPAGATE;
}
```

### 5.4 Integration with Zbus

The network module's task integrates SMF with zbus:

```c
static void network_task(void)
{
    struct network_state state = {0};
    
    /* Initialize state machine */
    smf_set_initial(SMF_CTX(&state), &states[STATE_RUNNING]);
    
    while (true) {
        /* Wait for message from any subscribed channel */
        int err = zbus_sub_wait_msg(
            &network_subscriber,
            &state.chan,
            state.msg_buf,
            K_FOREVER
        );
        
        if (err == -ENOMSG) {
            continue;
        }
        
        /* Run state machine with the message */
        err = smf_run_state(SMF_CTX(&state));
        if (err) {
            LOG_ERR("State machine error: %d", err);
            SEND_FATAL_ERROR();
        }
    }
}
```

**Key points:**
- State machine context includes zbus channel and message buffer
- `zbus_sub_wait_msg()` blocks until message arrives
- `smf_run_state()` processes the message through the state machine
- State machine decides how to handle message based on current state

## 6. Practical Example: Adding a New State

### 6.1 Scenario

Let's add a new state to the network module:

**Requirement:** Add a `STATE_DISCONNECTED_BACKOFF` state that waits before retrying connection after a failure.

### 6.2 Step 1: Define the State

Add to the state enumeration:

```c
enum network_states {
    STATE_RUNNING,
    STATE_DISCONNECTED,
    STATE_DISCONNECTED_IDLE,
    STATE_DISCONNECTED_SEARCHING,
    STATE_DISCONNECTED_BACKOFF,     /* NEW */
    STATE_CONNECTED,
    STATE_DISCONNECTING,
};
```

### 6.3 Step 2: Implement State Functions

```c
static void state_disconnected_backoff_entry(void *obj)
{
    struct network_state *state = (struct network_state *)obj;
    
    LOG_WRN("Connection failed, backing off for %d seconds",
            state->backoff_seconds);
    
    /* Start backoff timer */
    k_work_schedule(&state->backoff_work,
                    K_SECONDS(state->backoff_seconds));
    
    /* Increase backoff for next time (exponential backoff) */
    state->backoff_seconds = MIN(state->backoff_seconds * 2,
                                 MAX_BACKOFF_SECONDS);
}

static enum smf_state_result state_disconnected_backoff_run(void *obj)
{
    struct network_state *state = (struct network_state *)obj;
    
    if (state->chan == &NETWORK_CHAN) {
        struct network_msg *msg = (struct network_msg *)state->msg_buf;
        
        if (msg->type == NETWORK_BACKOFF_EXPIRED) {
            /* Backoff complete, try connecting again */
            smf_set_state(SMF_CTX(state),
                         &states[STATE_DISCONNECTED_SEARCHING]);
            return SMF_EVENT_CONSUMED;
        }
    }
    
    return SMF_EVENT_PROPAGATE;
}
```

### 6.4 Step 3: Add to State Table

```c
static const struct smf_state states[] = {
    /* ... existing states ... */
    
    [STATE_DISCONNECTED_BACKOFF] = SMF_CREATE_STATE(
        state_disconnected_backoff_entry,
        state_disconnected_backoff_run,
        NULL,
        &states[STATE_DISCONNECTED],    /* Parent */
        NULL
    ),
    
    /* ... other states ... */
};
```

### 6.5 Step 4: Transition to New State

Modify `STATE_DISCONNECTED_SEARCHING` to use backoff on failure:

```c
static enum smf_state_result state_disconnected_searching_run(void *obj)
{
    struct network_state *state = (struct network_state *)obj;
    
    if (state->chan == &NETWORK_CHAN) {
        struct network_msg *msg = (struct network_msg *)state->msg_buf;
        
        if (msg->type == NETWORK_CONNECTED) {
            /* Success - reset backoff */
            state->backoff_seconds = INITIAL_BACKOFF_SECONDS;
            smf_set_state(SMF_CTX(state), &states[STATE_CONNECTED]);
            return SMF_EVENT_CONSUMED;
        }
        
        if (msg->type == NETWORK_CONNECTION_TIMEOUT) {
            /* Failure - enter backoff state */
            smf_set_state(SMF_CTX(state),
                         &states[STATE_DISCONNECTED_BACKOFF]);
            return SMF_EVENT_CONSUMED;
        }
    }
    
    return SMF_EVENT_PROPAGATE;
}
```

### 6.6 Result

The new state diagram:

```
STATE_RUNNING
├── STATE_DISCONNECTED
│   ├── STATE_DISCONNECTED_IDLE
│   ├── STATE_DISCONNECTED_SEARCHING
│   │   └─── (timeout) ──► STATE_DISCONNECTED_BACKOFF
│   └── STATE_DISCONNECTED_BACKOFF
│       └─── (backoff expired) ──► STATE_DISCONNECTED_SEARCHING
├── STATE_CONNECTED
└── STATE_DISCONNECTING
```

## 7. Best Practices

### 7.1 State Design

✅ **DO:**
- Keep states focused on a single concern
- Use hierarchical states for common behavior
- Name states clearly and descriptively
- Document state transitions

❌ **DON'T:**
- Create too many states (keep it manageable)
- Mix unrelated concerns in one state
- Create deeply nested hierarchies (2-3 levels is usually enough)

### 7.2 State Functions

✅ **DO:**
- Keep run functions fast (run-to-completion)
- Use entry functions for initialization
- Use exit functions for cleanup
- Return `SMF_EVENT_CONSUMED` when message is handled

❌ **DON'T:**
- Block or sleep in state functions
- Perform long operations (split them up)
- Forget to handle return values
- Continue execution after calling `smf_set_state()`

### 7.3 State Transitions

✅ **DO:**
- Make state transitions explicit
- Clean up before transitioning
- Log state transitions for debugging
- Make transitions the last operation in run function

❌ **DON'T:**
- Transition from entry/exit functions
- Create transition loops (infinite loops)
- Forget to handle error cases
- Execute code after `smf_set_state()`

### 7.4 Testing

✅ **DO:**
- Test each state independently
- Test all possible transitions
- Test parent state event handling
- Use state logging to debug

❌ **DON'T:**
- Assume state transitions work without testing
- Forget edge cases and error conditions
- Skip integration testing

## 8. Debugging State Machines

### 8.1 Enable State Logging

Add logging to state transitions:

```c
static void log_state_transition(const char *from, const char *to)
{
    LOG_INF("State transition: %s -> %s", from, to);
}

static enum smf_state_result state_example_run(void *obj)
{
    /* ... */
    
    if (should_transition) {
        log_state_transition("EXAMPLE", "NEW_STATE");
        smf_set_state(SMF_CTX(obj), &states[STATE_NEW]);
        return SMF_EVENT_CONSUMED;
    }
    
    return SMF_EVENT_PROPAGATE;
}
```

### 8.2 Visualize State Machines

The ATT repository includes a tool to convert SMF code to PlantUML diagrams:

```bash
python scripts/smf_to_plantuml.py src/modules/network/network.c > network.puml
```

This generates a visual diagram you can use for documentation and debugging.

### 8.3 Common Issues

**Problem:** State machine stuck in a state
- **Cause:** No transition conditions are met
- **Solution:** Add logging to see which messages arrive, check transition logic

**Problem:** Unexpected state transitions
- **Cause:** Parent state handling message instead of child
- **Solution:** Return `SMF_EVENT_CONSUMED` from child state

**Problem:** State machine not responding
- **Cause:** Run function not being called
- **Solution:** Check zbus subscription and message flow

## Summary

In this lesson, you learned:

✅ State machines provide structure for complex module behavior
✅ SMF uses a run-to-completion model for predictable execution
✅ States have entry, run, and exit functions
✅ Hierarchical states allow sharing common behavior
✅ SMF integrates naturally with zbus messaging
✅ State transitions should be explicit and properly sequenced
✅ Proper state machine design improves maintainability and testability

## Exercises

### Exercise 1: Analyze the Network Module State Machine

Trace through the network module's state machine and document all possible state transitions.

See [Exercise 1 instructions](../exercises/lesson3/exercise1/README.md)

### Exercise 2: Add a New State

Add a backoff state to one of the ATT modules following the example in Section 6.

See [Exercise 2 instructions](../exercises/lesson3/exercise2/README.md)

## Next Steps

In the next lesson, you'll explore the **Core Modules** in detail - understanding what each module does, how they interact, and how to configure them for your application.

Continue to [Lesson 4: Working with Core Modules](lesson4_core_modules.md)

