# Lesson 1, Exercise 1: Build, Flash, and Provision Your First Application

## Objective

In this exercise, you will build the Asset Tracker Template from source, flash it to your hardware, and provision it with nRF Cloud. This exercise ensures your development environment is properly configured and that you understand the basic workflow.

## Prerequisites

- nRF Connect SDK v3.1.0 or later installed
- nRF Connect for VS Code extension installed
- Asset Tracker Template repository cloned
- Thingy:91 X or nRF9151 DK hardware
- nRF Cloud account created

## Steps

### Step 1: Initialize Workspace

If you haven't already, initialize the Asset Tracker Template workspace:

```bash
# Install nRF Util (if not already installed)
pip install nrfutil

# Install toolchain
nrfutil toolchain-manager install --ncs-version v3.1.0

# Launch toolchain environment
nrfutil toolchain-manager launch --ncs-version v3.1.0 --shell

# Initialize workspace
west init -m https://github.com/nrfconnect/Asset-Tracker-Template.git --mr main asset-tracker-template

# Navigate to project
cd asset-tracker-template/project/app

# Update dependencies
west update
```

### Step 2: Build the Firmware

Choose the build command for your hardware:

**For Thingy:91 X:**
```bash
cd asset-tracker-template/project/app
west build --pristine --board thingy91x/nrf9151/ns
```

**For nRF9151 DK:**
```bash
cd asset-tracker-template/project/app
west build --pristine --board nrf9151dk/nrf9151/ns
```

**Expected output:**
```
...
[165/165] Linking C executable zephyr/zephyr.elf
Memory region         Used Size  Region Size  %age Used
           FLASH:      487236 B       960 KB     49.55%
             RAM:      125632 B       320 KB     38.35%
        IDT_LIST:          0 GB         2 KB      0.00%
```

The build should complete without errors.

### Step 3: Flash the Firmware

**For Thingy:91 X (using serial bootloader):**
```bash
west thingy91x-dfu
```

**For Thingy:91 X or nRF9151 DK (with external debugger):**
```bash
west flash --erase
```

**Expected output:**
```
-- west flash: rebuilding
...
-- west flash: flashing
-- runners.nrfjprog: Flashing file: zephyr.hex
...
-- runners.nrfjprog: Board with serial number XXXXXXXXX flashed successfully.
```

### Step 4: Monitor Serial Output

Connect to the device's serial port:

```bash
# Using screen (Linux/Mac)
screen /dev/ttyACM0 115200

# Using minicom (Linux)
minicom -D /dev/ttyACM0 -b 115200

# Or use nRF Connect for Desktop Serial Terminal
```

**Expected output:**
```
*** Booting nRF Connect SDK v3.1.0 ***
[00:00:00.123,000] <inf> main: Asset Tracker Template started
[00:00:00.456,000] <inf> network: Initializing modem
[00:00:02.789,000] <inf> network: Searching for LTE network...
[00:00:15.012,000] <inf> network: Connected to LTE
[00:00:16.234,000] <inf> cloud: Connecting to nRF Cloud...
```

### Step 5: Obtain Attestation Token

On first boot, the device will print the attestation token automatically:

```
[00:00:20.567,000] <inf> cloud: Device not claimed
[00:00:20.568,000] <inf> cloud: Attestation token:
bDpCMTg5...very_long_token...9nQiK1JRPgo=
```

**Copy this entire token** - you'll need it for provisioning.

Alternatively, you can request it via the shell:

```bash
uart:~$ at at%attesttoken
```

### Step 6: Claim Device in nRF Cloud

1. Open your web browser and navigate to [nrfcloud.com](https://nrfcloud.com)
2. Log in to your nRF Cloud account
3. Navigate to **Security Services** → **Claimed Devices** in the left sidebar
4. Click **Claim Device**
5. Paste the attestation token into the **Claim token** field
6. Select the rule **"nRF Cloud Onboarding"**
   - If this rule doesn't exist, click **Create Rule**:
     - Name: `nRF Cloud Onboarding`
     - Action: `Claim and onboard to nRF Cloud`
     - Enable auto-provisioning: ✓
     - Click **Create**
7. Click **Claim Device**

**Expected result:**
```
Device successfully claimed
```

### Step 7: Verify Device Provisioning

Back in your serial monitor, you should see:

```
[00:00:25.123,000] <inf> cloud: Device claimed successfully
[00:00:26.456,000] <inf> cloud: Provisioning credentials...
[00:00:30.789,000] <inf> cloud: Credentials provisioned
[00:00:35.012,000] <inf> cloud: Connecting to nRF Cloud CoAP...
[00:00:40.234,000] <inf> cloud: Connected to nRF Cloud
[00:00:45.567,000] <inf> main: System initialized successfully
[00:00:45.568,000] <inf> main: Starting periodic sampling
```

### Step 8: View Device in nRF Cloud

1. In nRF Cloud, navigate to **Device Management** → **Devices**
2. Find your device in the list (identified by IMEI)
3. Click on the device to view its details

**You should see:**
- ✅ Device status: **Online**
- ✅ Last connection time (recent)
- ✅ App version
- ✅ Modem firmware version

### Step 9: Wait for First Data Sample

The device will sample location and sensors after initialization:

**Device logs:**
```
[00:01:00.123,000] <inf> main: Starting sample cycle
[00:01:00.456,000] <inf> location: Requesting GNSS location
[00:01:00.789,000] <inf> location: Waiting for GNSS fix...
[00:01:35.012,000] <inf> location: Location acquired (lat: 63.4305, lon: 10.3951)
[00:01:36.234,000] <inf> environmental: Sampling sensors
[00:01:36.567,000] <inf> environmental: Temperature: 22.5°C, Humidity: 45.2%, Pressure: 101.3 kPa
[00:01:37.890,000] <inf> cloud: Sending data to nRF Cloud
[00:01:39.123,000] <inf> cloud: Data sent successfully
[00:01:40.456,000] <inf> main: Sample cycle complete
```

### Step 10: View Data in nRF Cloud

Back in the nRF Cloud web UI:

1. Refresh the device page
2. You should now see:
   - **Location** on the map
   - **Environmental data** (temperature, humidity, pressure)
   - **Battery status**
   - **Connection information**

**Example data view:**
```
Location: 63.4305°N, 10.3951°E
Temperature: 22.5°C
Humidity: 45.2%
Pressure: 101.3 kPa
Battery: 100%
Last Update: Just now
```

## Verification Checklist

Verify you have completed all steps:

- [ ] Workspace initialized and dependencies updated
- [ ] Firmware built successfully
- [ ] Firmware flashed to device
- [ ] Serial output monitored
- [ ] Attestation token obtained
- [ ] Device claimed in nRF Cloud
- [ ] Device provisioned with credentials
- [ ] Device connected to nRF Cloud
- [ ] Device appears in nRF Cloud Devices list
- [ ] Location and sensor data visible in nRF Cloud

## Troubleshooting

### Build Fails

**Problem:** Build errors or missing dependencies

**Solution:**
- Ensure nRF Connect SDK is properly installed
- Run `west update` to fetch all dependencies
- Check that you're using the correct board target

### Device Not Connecting to LTE

**Problem:** Network connection fails

**Solution:**
- Ensure SIM card is installed and activated (if required)
- Check antenna connection
- Verify network coverage in your area
- Check logs for error messages:
  ```bash
  uart:~$ at at+cereg?
  ```

### Device Not Connecting to Cloud

**Problem:** Cloud connection fails after LTE is connected

**Solution:**
- Verify device was claimed in nRF Cloud
- Check that provisioning completed successfully
- Ensure nRF Cloud account is active
- Check cloud module logs for errors

### No Location Fix

**Problem:** GNSS cannot acquire location

**Solution:**
- Ensure clear sky view (near window or outdoors)
- Wait longer (first fix can take 60+ seconds)
- Check if A-GNSS is enabled:
  ```kconfig
  CONFIG_NRF_CLOUD_AGNSS=y
  ```
- Consider using Wi-Fi or cellular fallback

### Data Not Appearing in nRF Cloud

**Problem:** Device connected but no data visible

**Solution:**
- Wait for the first sampling cycle (may take a few minutes)
- Check device logs to confirm data is being sent
- Refresh the nRF Cloud device page
- Verify you're logged into the correct nRF Cloud account

## Expected Results

After completing this exercise, you should have:

✅ Successfully built the Asset Tracker Template firmware
✅ Flashed the firmware to your hardware
✅ Provisioned the device with nRF Cloud
✅ Verified the device is online and sending data
✅ Viewed location and sensor data in nRF Cloud web UI
✅ Basic understanding of the development workflow

## Next Steps

Now that you have the basic system running:

1. Observe the device behavior over time
2. Try pressing the button to trigger an immediate sample
3. Explore the nRF Cloud web UI features
4. Review the device logs to understand the operation

Continue to **Lesson 2** to learn about the zbus communication architecture.

## Additional Notes

### Power Consumption

The device uses LTE Power Saving Mode (PSM) by default. After sending data:
- Device enters deep sleep (PSM)
- Current consumption: ~5-15 µA
- Wakes periodically for sampling (configured interval)

### Configuration

You can modify the sampling interval in `prj.conf`:

```kconfig
CONFIG_APP_SENSOR_SAMPLING_INTERVAL_SECONDS=300  # 5 minutes
```

### Shell Commands

Try these shell commands to interact with the device:

```bash
# Check network status
uart:~$ at at+cereg?

# Check signal quality
uart:~$ at at+cesq

# List available commands
uart:~$ help

# Check kernel version
uart:~$ kernel version
```

## References

- [Asset Tracker Template Documentation](https://docs.nordicsemi.com/bundle/asset-tracker-template-latest/)
- [Getting Started Guide](https://docs.nordicsemi.com/bundle/asset-tracker-template-latest/page/common/getting_started.html)
- [nRF Cloud Documentation](https://docs.nordicsemi.com/bundle/nrf-cloud/)
- [nRF Connect SDK Installation](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/installation.html)

Congratulations on completing Exercise 1! You're now ready to explore the architecture in Lesson 2.

