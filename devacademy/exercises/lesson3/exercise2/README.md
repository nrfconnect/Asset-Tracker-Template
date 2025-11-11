# Lesson 3, Exercise 2: Add a New State

## Objective

Add a backoff state to the network module to implement exponential backoff after connection failures.

## Task Description

Add `STATE_DISCONNECTED_BACKOFF` that:
1. Waits before retrying connection
2. Uses exponential backoff (doubles wait time on each failure)
3. Caps at maximum backoff time
4. Resets backoff on successful connection

## Implementation Guide

See Lesson 3, Section 6 for detailed implementation steps.

Key additions:
- New state enum value
- Backoff timer work item
- State entry/run/exit functions
- Transition logic from SEARCHING state
- Backoff time tracking in state object

## Testing

1. Block LTE connection (airplane mode or remove SIM)
2. Observe backoff behavior:
   - First retry: 10 seconds
   - Second retry: 20 seconds
   - Third retry: 40 seconds
   - Fourth retry: 80 seconds
   - Caps at 300 seconds (5 minutes)

3. Restore connection
4. Verify backoff resets on success

See `solution/` directory for complete implementation.

