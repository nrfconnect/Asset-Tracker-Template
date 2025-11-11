# Lesson 5, Exercise 2: Implement Data Buffering

## Objective

Configure and test the storage module's buffering capability for offline operation.

## Task Description

1. Configure storage module for buffering
2. Simulate offline operation
3. Verify data is stored locally
4. Restore connection
5. Verify buffered data is sent

## Configuration

Edit `prj.conf`:

```kconfig
# Enable storage
CONFIG_APP_STORAGE=y
CONFIG_APP_STORAGE_FLASH=y

# Configure buffer
CONFIG_APP_STORAGE_MAX_ITEMS=50
CONFIG_APP_STORAGE_FLASH_SIZE=16384
```

## Testing Procedure

### Step 1: Baseline Test
1. Build and flash
2. Verify normal operation with cloud connected
3. Observe data flowing to cloud

### Step 2: Simulate Offline
1. Enable airplane mode: `at at+cfun=4`
2. Or disconnect from cloud in code
3. Continue sampling
4. Check logs for buffering messages

### Step 3: Verify Buffering
```
[storage] <inf> Cloud unavailable, buffering data
[storage] <inf> Buffered location data (1/50)
[storage] <inf> Buffered environmental data (2/50)
[storage] <inf> Buffered power data (3/50)
```

### Step 4: Restore Connection
1. Disable airplane mode: `at at+cfun=1`
2. Wait for cloud connection
3. Observe data transmission

### Step 5: Verify Data Sent
```
[storage] <inf> Cloud connected, sending buffered data
[storage] <inf> Sent item 1/3
[storage] <inf> Sent item 2/3
[storage] <inf> Sent item 3/3
[storage] <inf> All buffered data sent
```

## Verification

- [ ] Buffering activates when offline
- [ ] Data stored to flash
- [ ] Buffer doesn't overflow
- [ ] Data sent when online
- [ ] All data appears in nRF Cloud

## Bonus Challenges

1. **Fill the buffer** - Trigger many samples while offline
2. **Test buffer overflow** - Exceed MAX_ITEMS
3. **Verify persistence** - Reboot while offline, check data retained
4. **Monitor flash usage** - Track storage consumption

See `solution/` directory for detailed implementation.

