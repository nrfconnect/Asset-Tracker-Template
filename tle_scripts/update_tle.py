#!/usr/bin/env python3

import requests
import json
import time
import os
from datetime import datetime
import logging

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s'
)

# Configuration
CELESTRAK_URL = "https://celestrak.org/NORAD/elements/gp.php"
# SATELIOT_CATNR = "60550"  # SATELIOT_1
# SATELIOT_CATNR = "60552"  # SATELIOT_3
SATELIOT_CATNR = "60537"  # SATELIOT_4
NRF_CLOUD_API_URL = "https://api.nrfcloud.com/v1/devices"

def get_tle_from_celestrak(catnr):
    """Fetch TLE data from Celestrak for a specific satellite."""
    try:
        params = {
            'CATNR': catnr,
            'FORMAT': 'TLE'
        }
        response = requests.get(CELESTRAK_URL, params=params)
        response.raise_for_status()
        
        # Split response into lines and validate
        lines = response.text.strip().split('\n')
        if len(lines) != 3:
            raise ValueError("Invalid TLE data format")
        
        return {
            'name': lines[0].strip(),
            'line1': lines[1].strip(),
            'line2': lines[2].strip()
        }
    except Exception as e:
        logging.error(f"Error fetching TLE data: {e}")
        return None

def update_nrfcloud_shadow(tle_data, device_id, api_key):
    """Update nRFCloud device shadow with TLE data."""
    try:
        # Format data for nRFCloud shadow
        shadow_data = {
            "desired": {
                "tle": {
                    "name": tle_data['name'],
                    "line1": tle_data['line1'],
                    "line2": tle_data['line2']
                }
            }
        }

        headers = {
            'Authorization': f'Bearer {api_key}',
            'Content-Type': 'application/json'
        }

        # Update shadow
        url = f"{NRF_CLOUD_API_URL}/{device_id}/state"
        response = requests.patch(url, headers=headers, json=shadow_data)
        response.raise_for_status()
        logging.info("Successfully updated nRFCloud shadow")
        return True
    except Exception as e:
        logging.error(f"Error updating nRFCloud shadow: {e}")
        return False

def main():
    # Get API key and device ID from environment variables
    api_key = os.environ.get('NRF_CLOUD_API_KEY')
    device_id = os.environ.get('DEVICE_ID')
    
    if not api_key or not device_id:
        logging.error("NRF_CLOUD_API_KEY and DEVICE_ID environment variables must be set")
        return

    while True:
        logging.info("Starting TLE update cycle")
        
        # Fetch TLE data
        tle_data = get_tle_from_celestrak(SATELIOT_CATNR)
        if tle_data:
            logging.info(f"Got TLE data for {tle_data['name']}")
            
            logging.info(f"Updating TLE data for device {device_id}")

            # Update nRFCloud shadow
            if update_nrfcloud_shadow(tle_data, device_id, api_key):
                logging.info("TLE data successfully updated in nRFCloud")
            else:
                logging.error("Failed to update TLE data in nRFCloud")
        
        # Wait for one hour before next update
        logging.info("Waiting for next update cycle")
        time.sleep(3600)  # 1 hour in seconds

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        logging.info("Script terminated by user")
