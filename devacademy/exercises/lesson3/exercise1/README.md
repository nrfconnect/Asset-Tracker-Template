# Lesson 3, Exercise 1: Analyze the Network Module State Machine

## Objective

Thoroughly analyze the network module's state machine by tracing through the code, documenting all states and transitions, and creating a comprehensive state diagram.

## Task Description

1. Read the network module source code (`src/modules/network/network.c`)
2. Identify all states and their relationships
3. Document all possible transitions
4. Create a state diagram (using PlantUML, draw.io, or paper)
5. Trace through a complete connection sequence

## Steps

### Step 1: Identify States

List all states in the network module:
- STATE_RUNNING (parent)
- STATE_DISCONNECTED (parent)
- STATE_DISCONNECTED_IDLE
- STATE_DISCONNECTED_SEARCHING
- STATE_CONNECTED
- STATE_DISCONNECTING

### Step 2: Document State Functions

For each state, note:
- Entry function (if any)
- Run function (if any)
- Exit function (if any)
- Parent state
- Initial transition

### Step 3: Map State Transitions

Document transitions:
- From which state to which state
- What triggers the transition
- What actions are performed

Example:
```
STATE_DISCONNECTED_SEARCHING → STATE_CONNECTED
Trigger: NETWORK_CONNECTED message from modem
Actions: 
  - Exit STATE_DISCONNECTED_SEARCHING
  - Enter STATE_CONNECTED
  - Publish NETWORK_CONNECTED on NETWORK_CHAN
```

### Step 4: Create State Diagram

Use the provided PlantUML template or create your own diagram showing:
- All states
- All transitions with labels
- Parent-child relationships
- Initial states

### Step 5: Trace Connection Sequence

Document a complete connection sequence:

```
1. System starts
   - Initial state: STATE_RUNNING → STATE_DISCONNECTED → STATE_DISCONNECTED_SEARCHING

2. Modem starts searching
   - Entry function calls lte_lc_connect_async()
   
3. Network found
   - Modem callback triggers NETWORK_CONNECTED message
   - Transition to STATE_CONNECTED
   
4. Connection lost
   - Modem callback triggers NETWORK_DISCONNECTED message
   - Transition back to STATE_DISCONNECTED → STATE_DISCONNECTED_SEARCHING
```

## Deliverables

1. **State Documentation** - Markdown file describing each state
2. **Transition Table** - Table of all state transitions
3. **State Diagram** - Visual representation
4. **Sequence Trace** - Step-by-step trace of connection lifecycle

## Verification

- [ ] All 6 states documented
- [ ] All transitions identified
- [ ] Parent-child relationships understood
- [ ] State diagram created
- [ ] Connection sequence traced

## Expected Learning

After this exercise, you should:
- Understand the network module's complete behavior
- Know how hierarchical state machines work
- Recognize state machine patterns in other modules
- Be able to debug network connection issues

## Tools

**Generate state diagram from code:**
```bash
python scripts/smf_to_plantuml.py src/modules/network/network.c > network.puml
```

Then render with PlantUML online viewer or local tool.

## References

- [State Machine Framework Documentation](https://docs.zephyrproject.org/latest/services/smf/index.html)
- [Lesson 3: State Machine Framework](../../lessons/lesson3_state_machine_framework.md)
- [Network Module Documentation](../../../docs/modules/network.md)

See `solution/` directory for complete analysis.

