# Lesson 6, Exercise 2: Create a Custom Module

## Objective

Create a complete accelerometer module from scratch using the dummy template.

## Task Description

Follow the example in Lesson 6, Section 2.3 to create an accelerometer module that:
1. Samples acceleration data
2. Detects motion
3. Publishes motion events
4. Integrates with main module

## Implementation

See [Lesson 6, Section 2.3](../../lessons/lesson6_customization.md#23-creating-an-accelerometer-module) for complete step-by-step guide.

## Key Components

- Module interface (`accelerometer.h`)
- Module implementation (`accelerometer.c`)
- Configuration (`Kconfig.accelerometer`)
- Build integration (`CMakeLists.txt`)
- Main module integration

## Testing

1. Enable accelerometer
2. Move/shake the device
3. Observe motion detection
4. Verify events published
5. Check main module response

## Expected Behavior

```
[accel] <inf> Accelerometer module started
[accel] <inf> Motion detected
[main] <inf> Motion detected - trigger immediate sample
[accel] <inf> Motion stopped
```

## Verification

- [ ] Module compiles successfully
- [ ] Accelerometer sampled correctly
- [ ] Motion detection works
- [ ] Events published on zbus
- [ ] Main module responds appropriately

This is a comprehensive exercise that brings together all concepts from the course!

See `solution/` directory for complete implementation.

