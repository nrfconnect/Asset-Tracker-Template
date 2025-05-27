# TLE Update Script

This script fetches TLE (Two-Line Element) data for the SATELIOT-1 satellite from Celestrak and updates it in the nRF Cloud device shadow. It runs continuously, updating the TLE data every hour.

## Requirements

- Python 3.x
- `requests` library (`pip install requests`)

## Setup

1. Set the required environment variables:
   ```bash
   export NRF_CLOUD_API_KEY="your_api_key_here"
   export DEVICE_ID="your_device_id_here"
   ```

2. Make the script executable:
   ```bash
   chmod +x update_tle.py
   ```

## Usage

Run the script:
```bash
./update_tle.py
```

The script will:
1. Fetch TLE data from Celestrak for SATELIOT-1 (CATNR: 60550)
2. Update the device shadow in nRF Cloud with the new TLE data
3. Wait for one hour before the next update

To stop the script, press Ctrl+C.

## Example Shadow Update

The script updates the device shadow with TLE data in this format:
```json
{
  "desired": {
    "tle": {
      "name": "SATELIOT-1",
      "line1": "1 60550U 23082B   24015.51047488  .00000000  00000+0  00000+0 0  9990",
      "line2": "2 60550  97.4449 116.9749 0012345 266.7826  93.1978 15.12345678123456"
    }
  }
}
```

## Logging

The script logs its activity to stdout with timestamps. You can redirect the output to a file if needed:
```bash
./update_tle.py > tle_updates.log 2>&1
