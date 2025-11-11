# Lesson 4, Exercise 1: Configure Module Behavior

## Objective

Modify module configurations using Kconfig to change application behavior and observe the effects.

## Task Description

Make the following configuration changes and test each:

1. **Change sampling interval** from 5 minutes to 10 minutes
2. **Switch network mode** from LTE-M to NB-IoT
3. **Modify location timeout** from 60s to 120s
4. **Enable network quality metrics**
5. **Adjust PSM timers** for different power profiles

## Steps

### Task 1: Change Sampling Interval

Edit `prj.conf`:
```kconfig
CONFIG_APP_SENSOR_SAMPLING_INTERVAL_SECONDS=600  # 10 minutes
```

Build, flash, test:
- Observe 10-minute intervals between samples
- Check power consumption (should be lower)

### Task 2: Switch to NB-IoT

Edit `prj.conf`:
```kconfig
# Disable LTE-M
# CONFIG_LTE_NETWORK_MODE_LTE_M=y

# Enable NB-IoT
CONFIG_LTE_NETWORK_MODE_NBIOT=y
```

Build, flash, test:
- Connection may take longer
- Better coverage in some areas
- Lower power consumption

### Task 3: Increase Location Timeout

Edit `prj.conf`:
```kconfig
CONFIG_LOCATION_METHOD_GNSS_TIMEOUT=120000  # 120 seconds
```

Test:
- GNSS has more time to acquire fix
- Better success rate in challenging conditions

### Task 4: Enable Network Quality Metrics

Edit `prj.conf`:
```kconfig
CONFIG_APP_REQUEST_NETWORK_QUALITY=y
```

Test:
- RSRP values appear in device data
- Cell ID and area code logged

### Task 5: Adjust PSM Timers

For more aggressive power saving:
```kconfig
CONFIG_LTE_PSM_REQ_RPTAU="00010010"  # 18 hours
CONFIG_LTE_PSM_REQ_RAT="00000000"     # 0 seconds (immediate sleep)
```

For faster responsiveness:
```kconfig
CONFIG_LTE_PSM_REQ_RPTAU="00000001"  # 1 hour
CONFIG_LTE_PSM_REQ_RAT="00100001"    # 2 minutes active time
```

## Verification

For each change:
- [ ] Configuration edited correctly
- [ ] Firmware built successfully
- [ ] Behavior change observed
- [ ] Effects documented

## Expected Learning

- How Kconfig options control behavior
- Trade-offs between power and performance
- Impact of network mode selection
- Importance of timeout values

## References

- [Configuration Guide](../../../docs/common/configuration.md)
- [Lesson 4: Core Modules](../../lessons/lesson4_core_modules.md)

Try different combinations and document your observations!

