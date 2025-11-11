# Lesson 6, Exercise 1: Add a New Sensor

## Objective

Add support for the BMM350 magnetometer sensor to the environmental module.

## Task Description

Complete all steps from Lesson 6, Section 1.2 to:
1. Enable BMM350 in device tree
2. Extend environmental module
3. Sample magnetometer data
4. Update message structure
5. Integrate with cloud module
6. Test and verify

## Detailed Steps

See [Lesson 6, Section 1.2](../../lessons/lesson6_customization.md#12-example-adding-bmm350-magnetometer) for complete implementation guide.

## Verification

Test on Thingy:91 X (or board with BMM350):
- [ ] Sensor sampled successfully
- [ ] Data logged correctly
- [ ] Data sent to nRF Cloud
- [ ] Magnetometer values appear in cloud UI

## Expected Output

```
[env] <dbg> Magnetic field: X=45.23, Y=-12.45, Z=32.10 µT
[cloud] <dbg> Magnetometer data sent to cloud
```

See `solution/` directory for complete code.

